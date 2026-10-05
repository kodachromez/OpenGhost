# `native_fake_ui_smoke`: splash retirement race

Investigated from `cpp-native-extraction` at
`ce4cbcec65636679949668be3f5a0e50aabab63c`, on Linux x86-64 / Qt 6.11.2.
Release (`-O3 -DNDEBUG -g1` for backtrace line numbers), browser ON, CTest's
normal offscreen/software backend and reduced motion OFF. `QSG_RENDER_LOOP`
was **unset** at launch: the existing Linux default selects `threaded`.

## Reproduction and evidence

The unmodified fake smoke passed 15 consecutive CTest runs, then crashed on
run 8 of a bounded GDB repetition. Two subsequent 30-run GDB batches passed;
timing-only repetition is a weak regression oracle for this bug.

The original crash was **during the splash handoff, not application shutdown**:

```text
Thread 23 "QSGSoftwareRend" received signal SIGSEGV
#0 QQuickItem::window() const
#1 [libQt6QuickShapes.so.6]
#2 QSGSoftwareRenderableNode::renderNode(QPainter*, bool)
#3 QSGAbstractSoftwareRenderer::renderNodes(QPainter*)
#4 QSGSoftwareRenderer::render()
#6 QQuickWindowPrivate::renderSceneGraph()

Thread 1 "openghost-nativ"
#3 QTest::qWait(...)
#4 smokeTest() at tests/smoke.cpp:108  [original splash-handoff loop]
#5 main() at src/main.cpp:155
```

A controlled regression using the **production `Splash.qml`** reproduced the
same crash. After synchronization, it queues `Loader.active = false` on the
GUI thread and flushes deferred deletion. The render thread waits on a bounded
semaphore before rendering that already-synchronized frame. The log confirms
the splash's `QPointer` is null before rendering resumes:

```text
Splash retirement before render: deleted = true
Thread 23 "QSGSoftwareRend" received signal SIGSEGV
#0 QQuickItem::window(this=0x5555567ff450)
   qquickitem.cpp:3010
#1 QQuickShapeSoftwareRenderNode::render(...)
   qquickshapesoftwarerenderer.cpp:254
#2 QQuickShapeSoftwareRenderNode::render(...)
   qquickshapesoftwarerenderer.cpp:249
#3 QSGSoftwareRenderableNode::renderNode(...)
   qsgsoftwarerenderablenode.cpp:240
#4 QSGAbstractSoftwareRenderer::renderNodes(...)
#5 QSGSoftwareRenderer::render(...)
#6 QSGRenderer::renderScene(...)
#7 QQuickWindowPrivate::renderSceneGraph(...)
#8 QSGSoftwareRenderThread::syncAndRender(...)
   qsgsoftwarethreadedrenderloop.cpp:463
```

Qt 6.11.2's `QQuickShapeSoftwareRenderNode` retains a raw `QQuickShape *m_item`.
Its `render()` dereferences `m_item->window()->rendererInterface()` (line 254),
then `m_item->window()` again for the painter resource. The software threaded
loop releases the GUI after sync, **before** calling `renderSceneGraph()`.
`Main.qml` deactivates the splash Loader at `finished`; detachment/deletion can
therefore invalidate the aura's GUI item while its render node still belongs
to the current frame. This is a render-thread **use-after-free / scenegraph
lifetime race** in Qt's software Shape path, not a fake-backend or WebEngine
shutdown failure. A single-threaded loop masks the invalid lifetime overlap.

Qt source references (matching the installed version):

- [Shape software renderer](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quickshapes/qquickshapesoftwarerenderer.cpp)
- [Software threaded loop](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/adaptations/software/qsgsoftwarethreadedrenderloop.cpp)
- [Software painted node](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/adaptations/software/qsgsoftwarepainternode.cpp)

## Scoped fix

Replace only the splash's software aura Shape with `SplashAura`, a
`QQuickPaintedItem` with the same radial gradient, size, color, opacity and scale
animation. Qt paints it into node-owned pixels during synchronization, while
the GUI is blocked. Rendering then draws those pixels without consulting the
retired item. Opacity/transform animation reuses the cached pixels; it does not
repaint the gradient each frame.

No delayed deletion heuristic, render-thread lock in production, retained splash,
render-loop override, frame cap or animation-timing change was added. The GPU
mist and high-resolution frame clock are unchanged. Other Shapes are outside
this investigation; this is not a Qt-wide fix or a 240 Hz benchmark.

The smoke retains its original automatic handoff assertions and adds the
bounded, controlled retirement above as a separate fixture. It asserts that a
requested threaded loop actually used a render thread and that deletion
preceded rendering. With an explicitly selected basic loop there is no
concurrent retirement to exercise; no test sets that loop.

## Validation

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -g1' \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build-release --parallel 4

env -u QSG_RENDER_LOOP ctest --test-dir build-release \
  -R '^native_fake_ui_smoke$' --repeat until-fail:50 --verbose

env -u QSG_RENDER_LOOP ctest --test-dir build-release \
  -R '^(native_contract_test|native_browser_test|native_ui_smoke|native_fake_ui_smoke)$' \
  --output-on-failure
```

- Fake smoke: **50/50 passed**, 687.32 seconds. All 50 logs confirm deletion
  between sync and render and successful rendering afterward.
- Normal focused set: **4/4 passed**, 30.58 seconds, one invocation.
- `git diff --check`: passed. No unrelated stress suites or visible/GPU tests.
- Existing offscreen Vulkan, fontconfig and WebEngine diagnostic messages still
  appear; no QML binding/load warnings or failed smoke assertions.

Raw local logs are retained under the ignored `build-release/investigation/`:
`baseline-gdb.log`, `regression-before-gdb-3.log`, `fix-repeat-50.log`, and
`focused.log`. The useful crash excerpts are preserved above rather than
committing generated binaries, downloaded Qt sources or entire thread dumps.
