'''Diff judge unittest module.'''

from tornado import testing
from tornado.ioloop import IOLoop, PollIOLoop
import PyExt
import Privilege
from StdChal import StdChal, STATUS_AC, STATUS_RE


class EvIOLoop(PollIOLoop):
    '''Tornado compatible ioloop interface.'''

    def initialize(self, **kwargs):
        '''Initialize.'''

        super().initialize(impl=PyExt.EvPoll(), **kwargs)


class DiffJugeCase(testing.AsyncTestCase):
    '''Run diff judge tests.'''

    def get_new_ioloop(self):
        IOLoop.configure(EvIOLoop)
        return IOLoop().instance()

    @testing.gen_test(timeout=60)
    def test_stdchal(self):
        '''Test g++, A + B problems.'''

        chal = StdChal(1, 'tests/testdata/test.cpp', 'g++', 'diff', \
            'tests/testdata/res', \
            [
                {
                    'in': 'tests/testdata/res/testdata/0.in',
                    'ans': 'tests/testdata/res/testdata/0.out',
                    'timelimit': 10000,
                    'memlimit': 256 * 1024 * 1024,
                }
            ] * 4, {})
        result_list = yield chal.start()
        self.assertEqual(len(result_list), 4)
        for result in result_list:
            _, _, status, _ = result
            self.assertEqual(status, STATUS_AC)

    @testing.gen_test(timeout=60)
    def test_runtime_error(self):
        '''Test g++, Runtime Error.'''

        chal = StdChal(2, 'tests/testdata/testre.cpp', 'g++', 'diff', \
            'tests/testdata/res', \
            [
                {
                    'in': 'tests/testdata/res/testdata/0.in',
                    'ans': 'tests/testdata/res/testdata/0.out',
                    'timelimit': 10000,
                    'memlimit': 256 * 1024 * 1024,
                }
            ] * 1, {})
        result_list = yield chal.start()
        self.assertEqual(len(result_list), 1)
        for result in result_list:
            _, _, status, _ = result
            self.assertEqual(status, STATUS_RE)

def setUpModule():
    Privilege.init()
    PyExt.init()
    StdChal.init()
