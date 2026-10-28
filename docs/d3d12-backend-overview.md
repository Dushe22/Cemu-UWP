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
- `D3D12_QUEUE_RECORDING_AND_RETURN` and `D3D12_QUEUE_RECORDING_KIND_AND_RETURN` return from the calling function when they queue the operation. Their names must keep `AND_RETURN`; statements below them run only on the inline path. `QueueRecording` itself does not return from its caller.
- Objects freed by the core are queued and released in order on the recorder side (`texture_destroyView`, `texture_destroy`, `FlushPendingDestroys`).
- `SwapBuffers` keeps Latte at most one frame ahead (`WaitForQueuedSwaps`).

## Files

`D3D12Renderer.cpp` holds the includes, file-local helpers, device creation, initialization, shutdown and the swap chain. The rest of the class is in parts that it includes at the end (same translation unit, so file-local helpers stay shared). `D3D12RendererInternal.h` holds the shared includes and recording macros; the parts that need no file-local state compile on their own:

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
| `D3D12RendererImgui.cpp` (own translation unit) | ImGui frames and textures |
| `D3D12RendererRecorder.cpp` (own translation unit) | Render target state, two-phase destroys, recorder threads |
| `D3D12RendererTextures.inl` | Texture uploads, clears, copies, readbacks, surface copies |
| `D3D12RendererDraw.inl` | Buffer cache, streamout, draw sequences, `draw_execute`, index data, query objects |

Other files: `D3D12RecordRing.h` (operation ring), `D3D12PipelineRecipe/StableCache/PrewarmPolicy` (persistent pipeline cache), `D3D12HlslPruner` (removes unused HLSL before DXC), `D3D12MemoryManager`, `LatteTexture(View)D3D12`, `TextureReadbackD3D12`, `D3D12GpuWatchdog`.

## Measuring

Settings > Graphics > "Detailed performance logs" (applies when a game starts) writes CPU-time splits to `log.txt` every 5 s: emulated CPU cores, HLE calls and thread waits, the Latte thread, sequence and draw head, index decode, buffer sync, texture checks, recorder stages. Leave it off when not measuring; it costs a few percent.

## Tests and CI

`tools\tests\run_all.bat` builds and runs the isolated tests (record ring with one and two stages, pipeline prewarm policy, HLSL pruner byte-identical DXIL). `.github/workflows/uwp_checks.yml` runs them and a clang-format check of changed lines on pushes to `experimental`; the full UWP build is a manual run. `-DCEMU_UWP_STATIC_ANALYSIS=ON` adds MSVC `/analyze` to the D3D12 sources.

## Draw extraction (0.3.5.24)

`draw_execute` delegates to `PrepareDrawHead`, which keeps the existing draw accounting, device check,
profile, exclusive logic-op scope and stream-out finalizer alive through queuing and Latte bookkeeping.
`DecodeDrawIndices` decodes the guest indices before buffer synchronization. The head resolves the
shaders, pipeline identity, stencil policy, uniforms and register snapshot at the same points as before.

`DrawTailPacket` explicitly stores every former value capture of `recordDraw`: draw arguments,
decoded-index handle/range, shaders, pipeline identity, topology, cull/polygon state, target mask,
logic-op/stencil/stream-out decisions, uniform updates and the conditional profile/diagnostic flags.
The head initializes it at the old closure boundary. Register bytes still travel in the ring entry,
and shader/index pointers keep the existing queued lifetime; the packet does not own those objects.
Do not replace a frozen packet value with a later read from Latte or a renderer member.

`RecordDrawTail` applies the register delta and uniform updates, then runs `PrepareDrawTail`,
`EmitDrawTail` and `RetireDrawTail` on the same recorder thread (or inline under the same original rules).
`DrawTailState` holds only the temporary results shared by these helpers. Failed preparation or emission
returns to `RecordDrawTail`, whose finalizers still retire scratch and finish profiling.
`PrepareDrawTailTables` is also called by emission after feedback copies when those are required;
preparing tables earlier on that path would use the wrong SRVs. `FinishDrawOnLatte` tracks target writes
and queues the readback submit after the draw. Stream-out finish remains last on leaving the head.

## Memory translation-unit assessment (27 October 2026)

`D3D12RendererMemory.inl` can become a separate `.cpp`, but remains included for this change.
Its entire body is under `CEMU_UWP`; moving it must retain that guard, including scratch/copy methods.
The exact file-local dependencies outside the class are:

| Dependency in `D3D12Renderer.cpp` | Required treatment before moving |
|---|---|
| `kD3D12ScratchTexturePoolMaxEntries` (32), `kD3D12ScratchTexturePoolMaxBytes` (256 MiB) | Share one definition with scratch users; do not create independent budgets |
| `D3D12ScratchTextureDescMatches` | Share this pure full-resource-description comparison |
| `MixGameDescriptorTableKey` | Share the exact hash mixer with descriptor/copy caches |
| `D3D12TraceEvent` and its anonymous-namespace `D3D12TraceSink` | Preserve `game_pipeline_recipe_pressure_discard` in diagnostic builds; the current `D3D12_RENDERER_SEPARATE_PART` macro drops trace events, so using it unchanged would lose this event |

The remaining shared mutable state is **class-owned**, rather than file-local:

- Memory recovery shares pipeline/prewarm maps, their mutex/atomics, deferred-release queues,
  command-list/fence state, descriptor manager and upload allocator with Pipelines and Commands.
- Scratch pools, command scratch, feedback/retained copies, their lookup arrays and `m_activeFBO`
  are shared with Feedback, Draw, Textures and Commands; their recording order and fence retirement stay unchanged.
- The texture-copy cache and content generations are shared with Textures/Feedback/Draw.
  `FeedbackSourceGeneration` is already a renderer member defined in Feedback, so it only needs normal linkage.
- Function-local static `lastLoggedTrimPass` stays private to memory-pressure logging.

`LatteGPUState`, the core texture cache, `MemoryBudgetService` and `LatteUwpHitchScope` are declared
by existing headers; no new global state or copied texture cache is needed. A future move can use
the private header for pure helpers/constants and a diagnostic trace bridge, add the `.cpp` to
`src/Cafe/CMakeLists.txt`, and remove the include. This assessment adds no new synchronization or runtime behavior.

## Diagnostic texture copy ownership

`texture_copyImageSubData` queues copies only while `D3D12ShouldTrace()` is false. Copy traces call
`GetOrCreateView`, which registers views in Latte-owned lookup caches. With tracing active the copy runs
on Latte through the existing `D3D12_EXCLUSIVE_RECORDING` scope, after draining earlier commands.
Queuing that diagnostic work can recreate a view after the core detached its texture; deferred physical
destruction would then leave a lookup view pointing at a freed texture. Preserve this ownership rule
when changing copy scheduling. Non-diagnostic Release keeps its existing queued copy path.
