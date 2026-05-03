#if defined(GLINT_BUNDLE_DEEP)

#include "./glint_bundle_group_<!id!>.hpp"

namespace <!namespace!> {
namespace {

size_t s_offsets_<!id!>[<!offsets_arr_size!>] = {<!offsets!>};
size_t s_sizes_<!id!>[<!sizes_arr_size!>] = {<!sizes!>};
size_t s_data_size_<!id!> = <!data_size!>;
unsigned char s_data_<!id!>[<!data_size!>] = {<!data!>};

} // namespace

glint_bundle_group_<!id!>::glint_bundle_group_<!id!>() {
    getPointersCB = [](void** _offsets, void** _sizes, void** _data, size_t* _data_size) {
        *_offsets = (void*)s_offsets_<!id!>;
        *_sizes = (void*)s_sizes_<!id!>;
        *_data = (void*)s_data_<!id!>;
        *_data_size = s_data_size_<!id!>;
    };
}

} // namespace <!namespace!>

#endif