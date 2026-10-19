#include "UwpImGuiFrontend/Types.h"

#include <algorithm>

namespace UwpImGuiFrontend
{
const MediaAsset* FindMedia(const LibraryItem& item, MediaKind kind) noexcept
{
	const auto found = std::find_if(item.media.begin(), item.media.end(),
		[kind](const MediaAsset& media) { return media.kind == kind; });
	return found == item.media.end() ? nullptr : &*found;
}
}
