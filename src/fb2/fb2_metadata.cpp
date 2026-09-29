#include "fb2/fb2_metadata.h"

namespace fb2 {

std::string AuthorInfo::DisplayName() const {
    if (HasFullName()) {
        std::string result = first_name;
        if (!middle_name.empty()) {
            result += ' ';
            result += middle_name;
        }
        result += ' ';
        result += last_name;
        return result;
    }
    return nickname;
}

} // namespace fb2
