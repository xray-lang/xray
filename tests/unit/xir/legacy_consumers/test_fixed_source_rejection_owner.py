"""Negative binding controls only; no compiler execution is simulated as evidence."""
from pathlib import Path
import copy,json,tempfile,unittest
from unittest import mock
import fixed_source_rejection_owner as m
from fixed_product_inputs import FixedProductInputs


class FixedSourceOwnerControls(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.source=self.root/'source';self.consumer=self.root/'consumer';self.build=self.root/'producer';self.bundle=self.root/'product';self.owned=self.root/'owner'
        for p in (self.source,self.consumer,self.build,self.bundle,self.owned):p.mkdir()
        def put(root,name,data):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return path
        self.put=put;self.binary=put(self.bundle,'xray.exe',b'unit-input-image');put(self.build,'xray.exe',self.binary.read_bytes())
        self.stdlib=put(self.bundle,'stdlib/types/value.xr',b'unit input')
        self.owner=put(self.owned,'owner.exe',b'unit-owner');self.faults=put(self.owned,'faults.exe',b'unit-faults')
        put(self.consumer,'input.c',b'unit-consumer')
        put(self.source,'src/xir/xxir_checked.h',b'#define XR_XIR_CHECKED_SCHEMA 32u\n#define XR_XIR_CHECKED_CONTRACT 79u\n')
        put(self.source,'src/xir/xxir_value.h',b'#define XR_XIR_VALUE_ABI_VERSION 25u\n')
        for name in m.BUILD_FILES:put(self.build,name,b'unit graph '+name.encode())
        put(self.build,'CMakeCache.txt',('CMAKE_HOME_DIRECTORY:INTERNAL='+str(self.source)+'\n').encode())
        names=['xray_cli_source.lib','xray_xir_source_product.lib','xray_compiler_parser.lib','extra.lib']
        for name in names:put(self.build,name,('unit archive '+name).encode())
        self.link='link.exe /subsystem:console '+' '.join(names)+' kernel32.lib'
        self.version={'schema':1,'product':'xray-lang','version':'0.9.2','commit':'a'*40,'dirty':False,'target':'windows-x86_64','buildProfile':'Release','features':['vm','aot']}
        product={'schema':1,'source_commit':'a'*40,'binary':'xray.exe','binary_sha256':m.digest(self.binary.read_bytes()),'stdlib_root':'stdlib','version':self.version,'protocol':{'wire':32,'semantic':79,'value':25,'call':29,'program':34},'files':{'xray.exe':m.digest(self.binary.read_bytes()),'stdlib/types/value.xr':m.digest(self.stdlib.read_bytes())}}
        self.inputs=self.bundle/'inputs.json';self.inputs.write_text(json.dumps(product))
        self.fixed=FixedProductInputs(self.inputs,self.binary)
        def git(root,*args):
            if args==('status','--porcelain'):return ''
            if args==('rev-parse','HEAD'):return 'a'*40
            raise AssertionError(args)
        def command(argv,**kwargs):
            if argv[0]=='ninja':return self.link+'\n'
            if Path(argv[0])==self.binary:return json.dumps(self.version)
            raise AssertionError(argv)
        for patch in (mock.patch.object(m,'ROOT',self.consumer),mock.patch.object(m,'consumer_names',return_value={'input.c'}),
                      mock.patch.object(m,'producer_names',return_value=['src/xir/xxir_checked.h','src/xir/xxir_value.h']),
                      mock.patch.object(m,'git',side_effect=git),mock.patch.object(m.subprocess,'check_output',side_effect=command),
                      mock.patch.object(m.task_rejection_model,'expected',return_value={'scope':'UNIT_INPUT_ONLY'})):
            patch.start();self.addCleanup(patch.stop)
        self.data={'schema':3,'kind':'CANONICAL_CLI_SOURCE_REJECTION_OWNER','source_root':str(self.source),'code_head':'a'*40,'consumer_root':str(self.consumer),'consumer_head_at_build':'a'*40,'producer_build':str(self.build),'cli_binary':str(self.binary),'cli_version':copy.deepcopy(self.version),'cli_sha256':m.digest(self.binary.read_bytes()),'product_inputs_manifest':str(self.inputs),'product_inputs_manifest_sha256':m.digest(self.fixed.raw),'stdlib_path':str(self.fixed.stdlib),'owner_binary':str(self.owner),'owner_sha256':m.digest(self.owner.read_bytes()),'fault_owner_binary':str(self.faults),'fault_owner_sha256':m.digest(self.faults.read_bytes()),'consumer_inputs':m.file_table(self.consumer,{'input.c'}),'producer_inputs':m.file_table(self.source,m.producer_names(self.source)),'producer_build_inputs':m.file_table(self.build,m.BUILD_FILES),'link_inputs':m.link_inputs(self.build),'compiler_identities':m.compiler_identities(self.source)}
        self.manifest=self.root/'owner.json'

    def bind(self,data=None,fixed=None,binary=None):
        self.manifest.write_text(json.dumps(self.data if data is None else data))
        return m.FixedSourceOwner(binary or self.binary,self.owner,self.manifest,fixed or self.fixed)

    def test_duplicate_manifest_members_are_rejected(self):
        self.manifest.write_text('{"schema":2,"schema":3,"kind":"CANONICAL_CLI_SOURCE_REJECTION_OWNER"}')
        with self.assertRaises(ValueError):m.FixedSourceOwner(self.binary,self.owner,self.manifest,self.fixed)

    def test_separate_roots_and_complete_link_closure_bind(self):
        owner=self.bind();self.assertEqual(owner.verify()['compiler_identities'],{'checked_schema':32,'semantic_identity':79,'value_abi':25})
        self.assertEqual(len(owner.manifest['link_inputs']['archives']),4)
        self.assertNotEqual(owner.manifest['source_root'],owner.manifest['consumer_root'])

    def test_old_or_untyped_schema_never_falls_back(self):
        for schema in (1,2,True,'3'):
            bad=copy.deepcopy(self.data);bad['schema']=schema
            with self.subTest(schema=schema),self.assertRaises(ValueError):self.bind(bad)

    def test_missing_consumer_binding_is_rejected(self):
        bad=copy.deepcopy(self.data);bad['consumer_inputs']={}
        with self.assertRaises(ValueError):self.bind(bad)
        (self.consumer/'input.c').write_bytes(b'changed')
        with self.assertRaises(ValueError):self.bind()

    def test_changed_producer_header_is_rejected(self):
        (self.source/'src/xir/xxir_value.h').write_bytes(b'#define XR_XIR_VALUE_ABI_VERSION 24u\n')
        with self.assertRaises(ValueError):self.bind()

    def test_changed_ninja_graph_is_rejected(self):
        (self.build/'build.ninja').write_bytes(b'changed graph')
        with self.assertRaises(ValueError):self.bind()

    def test_changed_archive_payload_is_rejected(self):
        (self.build/'extra.lib').write_bytes(b'changed archive')
        with self.assertRaises(ValueError):self.bind()

    def test_wrong_compiler_protocol_is_rejected(self):
        bad=copy.deepcopy(self.data);bad['compiler_identities']['semantic_identity']=78
        with self.assertRaises(ValueError):self.bind(bad)

    def test_changed_owner_and_fault_images_are_rejected(self):
        self.owner.write_bytes(b'changed owner')
        with self.assertRaises(ValueError):self.bind()
        self.owner.write_bytes(b'unit-owner');self.faults.write_bytes(b'changed fault')
        with self.assertRaises(ValueError):self.bind()

    def test_wrong_product_manifest_hash_is_rejected(self):
        bad=copy.deepcopy(self.data);bad['product_inputs_manifest_sha256']='0'*64
        with self.assertRaises(ValueError):self.bind(bad)
        self.stdlib.write_bytes(b'changed library')
        with self.assertRaises(ValueError):self.bind()

    def test_same_bytes_at_another_cli_path_do_not_alias_the_product(self):
        alias=self.put(self.root,'alias.exe',self.binary.read_bytes())
        with self.assertRaises(ValueError):self.bind(binary=alias)

    def test_owner_manifest_mutation_after_binding_is_rejected(self):
        bound=self.bind();self.manifest.write_bytes(self.manifest.read_bytes()+b' ')
        with self.assertRaises(ValueError):bound.verify()

    def test_informational_consumer_commit_does_not_replace_file_binding(self):
        bad=copy.deepcopy(self.data);bad['consumer_head_at_build']='b'*40
        self.bind(bad).verify()
        bad['consumer_inputs']['input.c']='0'*64
        with self.assertRaises(ValueError):self.bind(bad)

    def test_dirty_producer_or_wrong_source_commit_is_rejected(self):
        bad=copy.deepcopy(self.data);bad['code_head']='b'*40
        with self.assertRaises(ValueError):self.bind(bad)
        with mock.patch.object(m,'git',side_effect=lambda root,*args:' M compiler.c' if args==('status','--porcelain') else 'a'*40),self.assertRaises(ValueError):self.bind()

    def test_wrong_paired_stdlib_path_is_rejected(self):
        bad=copy.deepcopy(self.data);bad['stdlib_path']=str(self.source/'stdlib')
        with self.assertRaises(ValueError):self.bind(bad)

    def test_incomplete_link_closure_is_rejected(self):
        bad=copy.deepcopy(self.data);bad['link_inputs']['archives'].pop(next(iter(bad['link_inputs']['archives'])))
        with self.assertRaises(ValueError):self.bind(bad)

    def test_missing_owner_stays_fail_closed_without_any_execution(self):
        import run_source_rejection_driver as driver
        with mock.patch.object(driver.task_rejection_model,'expected',return_value={'id':'unit','source':'fixture','source_lf_sha256':'0'*64,'qualification_scope':'UNIT'}),mock.patch.object(driver.proc,'run',side_effect=AssertionError('No execution authorized by missing binding')):
            result=driver.run_group(self.binary,None,None,None,self.root/'missing-owner')
        self.assertEqual(result['status'],'FAIL_MISSING_SOURCE_REJECTION_OWNER_OR_PROVIDER')
        self.assertEqual(result['commands'],[])


if __name__=='__main__':unittest.main()
