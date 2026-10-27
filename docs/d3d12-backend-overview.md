# D3D12 backend and recorder threads: overview for new developers

The Xbox/UWP fork renders with Direct3D 12 (`src/Cafe/HW/Latte/Renderer/D3D12/`). This page is a map; the details live in the code comments, `docs/uwp-development-plan.md` and `docs/phase5-multithreaded-recording-plan.md`.

## Threads

| Thread | Role |
|---|---|
| Emulated CPU (`OSSched[core=N]`, 3 cores) | Runs the game through the PPC recompiler; GX2 HLE calls write command buffers. |
| Latte (`LatteCommandProcessor`) | Parses the command buffers, keeps the GPU register file, resolves shaders, FBOs and textures for each draw sequence, and calls the renderer entry points (`draw_beginSequence`, `draw_execute`, texture/copy/clear calls). |
| `d3d12Record` (optional, "Threaded draw recording") | Records the D3D12 command lists. Latte pushes operations into `D3D12RecordRing` (single producer, single consumer, built in place); the draw head stays on Latte, the draw tail (pipeline lookup, descriptor tables, barriers, the draw) runs here. |
| `d3d12Prepare` (optional, "Two-stage recording", off) | Forwards operations to a second ring; meant to host the preparation half of the draw tail (phase 5 S2d, paused). |
| `d3d12AsyncPSO*`, `d3d12ShaderLoad`, `d3d12SliceUpload` | Background PSO creation, shader cache loading, texture slice writes. |

Rules that keep the threads correct:
- Anything that reads or changes Latte core state (texture cache, views, FBO cache, registers) runs on Latte. Only the D3D12 recording goes to the recorder.
- An entry point either queues an operation (`QueueRecording`, with a copy of the data it needs) or takes `RecordingExclusiveScope`, which drains the queue and runs inline.
- Objects freed by the core are queued and released in order on the recorder side (`texture_destroyView`, `texture_destroy`, `FlushPendingDestroys`).
- `SwapBuffers` keeps Latte at most one frame ahead (`WaitForQueuedSwaps`).

## Files

`D3D12Renderer.cpp` holds the includes, file-local helpers, device creation, initialization, shutdown and the swap chain. The rest of the class is in parts that it includes at the end (same translation unit, so file-local helpers stay shared):

| Part | Contents |
|---|---|
| `D3D12RendererDescriptors.inl` | Descriptor tables, their cache keys, uniform buffer upload |
| `D3D12RendererPipelines.inl` | Output/helper pipelines, point/rect geometry shaders, pipeline recipes, prewarm, async PSOs, `GetGamePipeline` |
| `D3D12RendererCommands.inl` | Command lists, fences, deferred releases, cached command state, resource transitions |
| `D3D12RendererMemory.inl` | Memory pressure, eviction, allocation recovery, scratch textures, copy tracking (UWP only) |
| `D3D12RendererFeedback.inl` | Feedback-loop copies, logic op emulation, framebuffer binding |
| `D3D12RendererDiagnostics.inl` | Texture/FBO/binding traces (diagnostic builds) |
| `D3D12RendererQueriesAndProfiling.inl` | Occlusion queries, device-removed checks, checked view creation, clears, UWP performance logs |
| `D3D12RendererOutput.inl` | Swap, backbuffer output, screenshots, stream capture, cached outputs for the UWP shell |
| `D3D12RendererImgui.inl` | ImGui frames and textures |
| `D3D12RendererRecorder.inl` | Render target state, two-phase destroys, recorder threads |
| `D3D12RendererTextures.inl` | Texture uploads, clears, copies, readbacks, surface copies |
| `D3D12RendererDraw.inl` | Buffer cache, streamout, draw sequences, `draw_execute`, index data, query objects |

Other files: `D3D12RecordRing.h` (operation ring), `D3D12PipelineRecipe/StableCache/PrewarmPolicy` (persistent pipeline cache), `D3D12HlslPruner` (removes unused HLSL before DXC), `D3D12MemoryManager`, `LatteTexture(View)D3D12`, `TextureReadbackD3D12`, `D3D12GpuWatchdog`.

## Measuring

Settings > Graphics > "Detailed performance logs" (applies when a game starts) writes CPU-time splits to `log.txt` every 5 s: emulated CPU cores, HLE calls and thread waits, the Latte thread, sequence and draw head, index decode, buffer sync, texture checks, recorder stages. Leave it off when not measuring; it costs a few percent.

## Tests and CI

`tools\tests\run_all.bat` builds and runs the isolated tests (record ring with one and two stages, pipeline prewarm policy, HLSL pruner byte-identical DXIL). `.github/workflows/uwp_checks.yml` runs them and a clang-format check of changed lines on pushes to `experimental`; the full UWP build is a manual run. `-DCEMU_UWP_STATIC_ANALYSIS=ON` adds MSVC `/analyze` to the D3D12 sources.
