"""Keep independent model inputs inside the delivered repository source tree."""
import unittest
from independent_complete_models import require_owned_input_paths


class OwnedInputPaths(unittest.TestCase):
    def test_owned_source_roots(self):
        require_owned_input_paths(['tests/fixtures/root.xr','stdlib/types/array.xr',
                                   'spec/source/section.md','src/xir/xxir.h'])

    def test_private_roots_and_nested_private_paths(self):
        for name in ['.tmp_p19/source.xr','.tmp/source.xr','tests/.tmp_private/source.xr','build/root.xr']:
            with self.subTest(name=name):
                with self.assertRaises(ValueError):require_owned_input_paths([name])

    def test_absolute_and_traversal_inputs(self):
        for name in ['/tmp/source.xr','E:/workspace/source.xr','../source.xr','tests/../source.xr']:
            with self.subTest(name=name):
                with self.assertRaises(ValueError):require_owned_input_paths([name])

    def test_noncanonical_inputs(self):
        for name in ['tests\\root.xr','tests//root.xr','tests/./root.xr','',None]:
            with self.subTest(name=name):
                with self.assertRaises(ValueError):require_owned_input_paths([name])


if __name__ == '__main__':unittest.main()
