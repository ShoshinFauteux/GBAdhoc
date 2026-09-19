import json
from pathlib import Path
import tempfile
import time
import unittest
import shutil
from unittest.mock import patch
import perf_loop as loop


class PerformanceLoopTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.device = self.base/'PSP/GAME/GBADHOC-PERF'
        self.package = self.base/'package'
        (self.device/'handoff').mkdir(parents=True)
        self.package.mkdir()
        (self.device/'RIG.ID').write_text('test-rig')
        (self.device/'handoff/RESULT.TXT').write_text('run=1\nexit=0\nstatus=ready\n')
        names = ['EBOOT.PBP', 'gbadhoc_me.prx', 'CONFIG.INI', '.gpsp-harness.ini', 'battle.inputs', 'roms/test.st0', 'roms/test.sav']
        for name in names:
            p = self.package/name
            p.parent.mkdir(exist_ok=True)
            p.write_bytes(('payload '+name).encode())
        self.job = {'id':'baseline_u_1', 'fixture':'unbound', 'from':300, 'to':900, 'core_phase':0,
                    'files':{n:loop.sha(self.package/n) for n in names}, 'resident':{}}
        loop.save_json(self.package/'job.json', self.job)
        self.log = '''EVT perf_job id=baseline_u_1 fixture=unbound from=300 to=900 timeout=180
EVT autoload_state rc=0
EVT me_rend on stage_bytes=42
EVT frame_dump file=frame_000091.bmp
EVT perf_window f=600 n=300 wall_us=5000000 work_us=3000000 wall_max=20000 work_max=15000 over=0 hist=200,100,0,0,0,0,0,0
EVT perf_window f=900 n=300 wall_us=6000000 work_us=4000000 wall_max=24000 work_max=20000 over=100 hist=0,200,100,0,0,0,0,0
EVT perf_done samples=600 expected=600
EVT ap_done steps=8 frame=930
EVT frame_dump file=frame_000901.bmp
EVT me_rend off reason=exit frames=930
'''

    def test_weighted_complete_score(self):
        s = loop.score(self.log, self.job, {'exit':'0'})
        self.assertTrue(s['scoring'], s)
        self.assertAlmostEqual(s['emu_fps'], 600e6/11000000)
        self.assertEqual(s['wall_histogram'], [200,300,100,0,0,0,0,0])

    def test_rejects_early_or_wrong_or_fallback_runs(self):
        changes = [('rc=0','rc=-1'), ('samples=600','samples=599'), ('f=900','f=901'),
                   ('n=300','n=299'), ('baseline_u_1','wrong'), ('EVT ap_done','EVT missing'),
                   ('reason=exit','reason=watchdog'), ('hist=200,100','hist=200,99')]
        for before, after in changes:
            with self.subTest(before=before):
                self.assertFalse(loop.score(self.log.replace(before, after),self.job,{'exit':'0'})['valid'])
        self.assertFalse(loop.score(self.log,self.job,{'exit':'6'})['valid'])

    def test_diagnostics_not_scores(self):
        self.job['core_phase'] = 2
        self.assertFalse(loop.score(self.log,self.job,{'exit':'0'})['valid'])
        s = loop.score(self.log+'EVT core_phase lvl=2 f=600\n',self.job,{'exit':'0'})
        self.assertTrue(s['valid'])
        self.assertFalse(s['scoring'])

    def test_audio_oracle_is_required_and_never_scored(self):
        self.job['audio_oracle'] = True
        self.job['scoring'] = False
        self.assertFalse(loop.score(self.log,self.job,{'exit':'0'})['valid'])
        s=loop.score(self.log+'EVT audio_hash deadbeef samples=131072\n',self.job,{'exit':'0'})
        self.assertTrue(s['valid'],s)
        self.assertFalse(s['scoring'])

    def test_stage_hashes_before_command_and_flushes_twice(self):
        calls = []
        def flush(root):
            calls.append((root/'handoff/CMD.TXT').exists())
            self.assertTrue(all(loop.sha(root/n)==h for n,h in self.job['files'].items()))
        self.assertTrue(loop.stage(self.package,self.device,'test-rig',time.monotonic()+100,flush))
        self.assertEqual(calls,[False,True])
        self.assertEqual((self.device/'handoff/CMD.TXT').read_text(),'RUN\n')

    def test_copy_or_flush_failure_never_authorizes_partial_package(self):
        for target in ['atomic','flush']:
            with self.subTest(target=target):
                with patch.object(loop,'atomic',side_effect=OSError('copy')) if target=='atomic' else patch.object(loop,'flush',side_effect=OSError('flush')):
                    with self.assertRaises(OSError):
                        loop.stage(self.package,self.device,'test-rig',time.monotonic()+100,loop.flush)
                self.assertFalse((self.device/'handoff/CMD.TXT').exists())

    def test_post_flush_corruption_never_authorizes_package(self):
        def corrupt_control_file(root):
            original_size = (root/'.gpsp-harness.ini').stat().st_size
            data = (root/'battle.inputs').read_bytes()
            (root/'.gpsp-harness.ini').write_bytes(
                data.ljust(original_size, b'\0'))
        with self.assertRaisesRegex(IOError,
                                   'post-flush staged hash mismatch'):
            loop.stage(self.package, self.device, 'test-rig',
                       time.monotonic()+100, corrupt_control_file)
        self.assertFalse((self.device/'handoff/CMD.TXT').exists())

    def test_late_window_is_deferred(self):
        self.assertFalse(loop.stage(self.package,self.device,'test-rig',time.monotonic()+35,lambda _:None))
        self.assertFalse((self.device/'EBOOT.PBP').exists())

    def test_partial_copy_and_command_flush_failure_stop_dispatch(self):
        real_atomic=loop.atomic
        writes=[]
        def copy(path,data):
            writes.append(path)
            if len(writes)==3: raise OSError('mid-copy failure')
            real_atomic(path,data)
        with patch.object(loop,'atomic',side_effect=copy):
            with self.assertRaises(OSError):
                loop.stage(self.package,self.device,'test-rig',time.monotonic()+100,lambda _:None)
        self.assertTrue((self.device/'EBOOT.PBP').exists())
        self.assertFalse((self.device/'handoff/CMD.TXT').exists())
        flushes=[]
        def flush(root):
            flushes.append((root/'handoff/CMD.TXT').exists())
            if len(flushes)==2: raise OSError('command flush failed')
        with self.assertRaises(OSError):
            loop.stage(self.package,self.device,'test-rig',time.monotonic()+100,flush)
        self.assertEqual(flushes,[False,True,False])
        self.assertFalse((self.device/'handoff/CMD.TXT').exists())

    def test_wrong_marker_hash_path_and_pending_command_refused(self):
        with self.assertRaises(ValueError): loop.stage(self.package,self.device,'wrong',time.monotonic()+100)
        for name in ['../evil','/abs','C:/elsewhere','roms/../evil','roms\\evil']:
            with self.assertRaises(ValueError): loop.safe_name(name)
        (self.package/'EBOOT.PBP').write_bytes(b'broken')
        with self.assertRaises(ValueError): loop.load_job(self.package)

    def test_complete_collect_stage_relaunch_cycle_and_park(self):
        campaign = self.base/'campaign'
        campaign.mkdir()
        second = self.base/'second'
        shutil.copytree(self.package,second)
        second_job = dict(self.job,id='baseline_u_2')
        loop.save_json(second/'job.json',second_job)
        loop.save_json(campaign/'campaign.json',{'device':str(self.device),'marker':'test-rig'})
        loop.save_json(campaign/'queue.json',['../package','../second'])
        for name in self.job['files']:
            dst=self.device/name
            dst.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(self.package/name,dst)
        loop.save_json(self.device/'JOB.JSON',self.job)
        (self.device/'log').mkdir()
        (self.device/'log/frontend.log').write_text(self.log)
        window=self.device/'handoff/WINDOW.TXT'
        window.write_text('token=1-1\nseconds=90\n')
        sleeps=[]
        def sleep(_):
            sleeps.append(1)
            if len(sleeps)==1:
                window.write_text('token=1-2\nseconds=90\n')
            journal=campaign/'journal.json'
            if journal.exists() and len(loop.read_json(journal)['completed'])==2:
                (campaign/'STOP').touch()
            if len(sleeps)>10: self.fail('controller stalled')
        def ps(_):
            # Simulated PSP consumes command and completes the next run.
            self.assertEqual((self.device/'handoff/CMD.TXT').read_text(),'RUN\n')
            (self.device/'handoff/CMD.TXT').unlink()
            (self.device/'handoff/RESULT.TXT').write_text('run=2\nexit=0\nreason=ok\nstatus=ready\n')
            (self.device/'log/frontend.log').write_text(self.log.replace('baseline_u_1','baseline_u_2'))
            window.write_text('token=2-1\nseconds=90\n')
        with patch.object(loop.time,'sleep',side_effect=sleep), patch.object(loop,'ps',side_effect=ps), patch.object(loop,'flush',return_value=None):
            # stage's default is bound at definition time, so explicitly inject
            # the fake volume flush while retaining its real staging logic.
            real_stage=loop.stage
            with patch.object(loop,'stage',side_effect=lambda *a:real_stage(*a,flush_fn=lambda _:None)):
                loop.run(campaign)
        journal=loop.read_json(campaign/'journal.json')
        self.assertEqual(journal['completed'],['1-baseline_u_1','2-baseline_u_2'])
        self.assertEqual(journal['dispatched'],['baseline_u_2'])


if __name__ == '__main__': unittest.main()
