#include "fb2/fb2_index.h"

namespace fb2 {

void DocumentIndex::clear() {
    blocks.clear();
    footnotes.clear();
    images.clear();
    toc_flat.clear();
    toc_root = TocEntry{};
    notes_body_start = 0;
}

} // namespace fb2
