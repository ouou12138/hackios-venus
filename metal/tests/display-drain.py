#!/usr/bin/env python3
"""Execute the actual adapted framebuffer drain with competing host threads."""
import ast
import importlib.util
import subprocess
import tempfile
from pathlib import Path
BASE=Path(__file__).resolve().parents[1]
reference=BASE/'tests'
spec=importlib.util.spec_from_file_location('fences',reference/'methods.py')
fences=importlib.util.module_from_spec(spec);spec.loader.exec_module(fences)
tree=ast.parse((reference/'gpu-return.py').read_text())
pre=next(ast.literal_eval(node.value) for node in tree.body if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in node.targets))
source=(BASE/'src/kernel/MiniIOMobileFramebuffer.cpp').read_text()
present=fences.method(source,'IOMobileFramebuffer::present')
assert present.count('while (fReturnStopping) IOLockSleep')==2
presenter=fences.method(source,'IOMobileFramebuffer::presenter')
assert presenter.index('fPresenterReading = true')<presenter.index('show(swap, surfaces)')<presenter.index('MiniIOSurfaceClearKept()')<presenter.index('fPresenterReading = false')
declaration='''
struct IOMobileFramebuffer {
 IOLock *fSwapLock; unsigned fSwapsWaiting; bool fPresenterReading, fReturnStopping;
 bool stopForReturn();
};
'''
post=r'''
int main(){
 IOLock lock; IOMobileFramebuffer fb{&lock,1,true,false};
 std::atomic<bool> complete=false;
 std::thread shutdown([&]{assert(fb.stopForReturn());complete=true;});
 for(;;){IOLockLock(&lock);bool closing=fb.fReturnStopping;IOLockUnlock(&lock);if(closing)break;std::this_thread::yield();}
 assert(!complete);
 IOLockLock(&lock);fb.fSwapsWaiting=0;IOLockWakeup(&lock,&fb.fReturnStopping,false);IOLockUnlock(&lock);
 assert(!complete); // retiring the queue does not release a presenter still reading
 IOLockLock(&lock);fb.fPresenterReading=false;IOLockWakeup(&lock,&fb.fReturnStopping,false);IOLockUnlock(&lock);
 shutdown.join();assert(complete&&fb.fReturnStopping);
 puts("PASS drain waits for both queued swaps and final source-reader retirement");
 immediateDeadline=true;fb.fSwapsWaiting=2;fb.fPresenterReading=true;
 assert(!fb.stopForReturn());assert(fb.fReturnStopping&&fb.fSwapsWaiting==2&&fb.fPresenterReading);
 puts("PASS timeout retains queued surfaces and reader state; admission stays closed");
 puts("PASS production present checks termination both before and after capacity waits; reader guard covers source releases");
}
'''
(BASE/'out/host-tests').mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=BASE/'out/host-tests',prefix='display-drain-test-') as work:
    work=Path(work);code=work/'test.cpp';code.write_text(pre+declaration+fences.method(source,'IOMobileFramebuffer::stopForReturn')+post)
    subprocess.run(['clang++','-std=c++17','-pthread','-fsanitize=address,undefined',str(code),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True,timeout=15)
