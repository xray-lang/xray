import unittest
import complete_scalar_events_model as model

class ScalarEventTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'ieee_arithmetic':[False,True,False,True,True,True,True,True,False,False],
         'contextual_numeric_conversion':[*([True]*9),'numeric conversion is out of range',422,'numeric conversion is out of range',422],
         'enum_ordinal_conversion':[1,1,1],'range_precedence_arithmetic':[5,4,6,7,10,20,30],
         'loop_invariant_rc_value':[6,22,21,8,18],
         'string_surface_declared_methods':[5,6,104,233,104,233,120,'cba','😀éa',42,-7,None,3.5,None,True,False,'self','hi','hé',True,'Utf8Error.InvalidUtf8','�','[104, 105]','é','🙂','a|β|中','aβ中','A','é','中',True,'StringSliceError.InvalidByteRange','[1, 2, 3]'],
         'unit_enum_return_value':[True,True,True,200,100,300,True,200]}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),79)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_ties_to_even_without_host_float_cast(self):
        for value,precision,expected in [(16777217,24,16777216),(16777219,24,16777220),((1<<64)-1,53,1<<64)]:
            with self.subTest(value=value):self.assertEqual(model.round_unsigned_to_precision(value,precision),expected)

    def test_float_integer_range_is_checked_after_truncation(self):
        self.assertEqual(model.float_to_unsigned(-0.99,8),0)
        for value in [256.0,-1.0,float('nan'),float('inf')]:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):model.float_to_unsigned(value,8)

    def test_utf8_optional_and_nominal_observations(self):
        self.assertIsNone(model.optional_int('12tail'));self.assertEqual(model.optional_int(' -7 '),-7)
        self.assertEqual(model.render(None),'null');self.assertEqual(model.numeric_array([104,105]),'[104, 105]')
        self.assertEqual(model.choose(1),('Color','Green'));self.assertNotEqual(model.choose(1),('Other','Green'))

if __name__=='__main__':unittest.main()
