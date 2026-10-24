#pragma once

#include <string>
#include <string_view>

namespace D3D12HlslPruner
{
// Removes top-level function definitions that nothing references. DXC parses and generates code for
// every function in the source, used or not, so the shared helper prelude the HLSL emitter puts in
// front of each shader costs more compile time than most shader bodies.
//
// Conservative by design: a referenced name keeps all its overloads, identifiers in comments and
// inactive preprocessor branches count as references, and anything that does not look like a plain
// function definition (attributes, templates, structs, cbuffers) is kept. Returns false and leaves
// output empty when nothing could be removed.
bool PruneUnusedFunctions(std::string_view source, std::string_view entryPoint, std::string& output);
}
