#include "raw_thumb_decoder.hpp"

#include "lvgl_wrap.hpp"
#include "raw_thumb_format.hpp"
#include "src/draw/lv_image_decoder_private.h"

#include <cstring>

namespace ui {

namespace {

lv_image_decoder_t* g_decoder = nullptr;

bool is_rthm_file(const lv_image_decoder_dsc_t* dsc)
{
    if(!dsc || dsc->src_type != LV_IMAGE_SRC_FILE || !dsc->src) {
        return false;
    }
    const char* ext = lv_fs_get_ext(static_cast<const char*>(dsc->src));
    return ext && lv_strcmp(ext, "rthm") == 0;
}

bool read_thumb_header(const char* path, upc::RawThumbHeader& out)
{
    if(!path || !path[0]) {
        return false;
    }
    lv_fs_file_t file;
    if(lv_fs_open(&file, path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        return false;
    }
    uint32_t br = 0;
    const lv_fs_res_t res = lv_fs_read(&file, &out, sizeof(out), &br);
    (void)lv_fs_close(&file);
    if(res != LV_FS_RES_OK || br != sizeof(out)) {
        return false;
    }
    return upc::raw_thumb_header_valid(out);
}

lv_result_t decoder_info(lv_image_decoder_t* /*decoder*/,
                         lv_image_decoder_dsc_t* dsc,
                         lv_image_header_t* header)
{
    if(!is_rthm_file(dsc) || !header) {
        return LV_RESULT_INVALID;
    }

    upc::RawThumbHeader h{};
    if(!read_thumb_header(static_cast<const char*>(dsc->src), h)) {
        return LV_RESULT_INVALID;
    }

    header->cf = LV_COLOR_FORMAT_RGB565;
    header->w = static_cast<int32_t>(h.width);
    header->h = static_cast<int32_t>(h.height);
    header->stride = h.stride_bytes;
    header->flags = LV_IMAGE_FLAGS_MODIFIABLE;
    return LV_RESULT_OK;
}

lv_result_t decoder_open(lv_image_decoder_t* /*decoder*/,
                         lv_image_decoder_dsc_t* dsc)
{
    if(!is_rthm_file(dsc)) {
        return LV_RESULT_INVALID;
    }

    const char* path = static_cast<const char*>(dsc->src);
    upc::RawThumbHeader h{};
    if(!read_thumb_header(path, h)) {
        return LV_RESULT_INVALID;
    }

    lv_fs_file_t file;
    if(lv_fs_open(&file, path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        return LV_RESULT_INVALID;
    }

    lv_draw_buf_t* decoded = lv_draw_buf_create(
        h.width, h.height, LV_COLOR_FORMAT_RGB565, h.stride_bytes);
    if(!decoded) {
        (void)lv_fs_close(&file);
        return LV_RESULT_INVALID;
    }

    if(decoded->data_size < h.data_size) {
        lv_draw_buf_destroy(decoded);
        (void)lv_fs_close(&file);
        return LV_RESULT_INVALID;
    }

    uint32_t br = 0;
    bool ok = lv_fs_seek(&file, sizeof(h), LV_FS_SEEK_SET) == LV_FS_RES_OK &&
              lv_fs_read(&file, decoded->data, h.data_size, &br) == LV_FS_RES_OK &&
              br == h.data_size;
    (void)lv_fs_close(&file);
    if(!ok) {
        lv_draw_buf_destroy(decoded);
        return LV_RESULT_INVALID;
    }

    lv_draw_buf_t* adjusted = lv_image_decoder_post_process(dsc, decoded);
    if(!adjusted) {
        lv_draw_buf_destroy(decoded);
        return LV_RESULT_INVALID;
    }
    if(adjusted != decoded) {
        lv_draw_buf_destroy(decoded);
        decoded = adjusted;
    }

    dsc->decoded = decoded;
    return LV_RESULT_OK;
}

void decoder_close(lv_image_decoder_t* /*decoder*/,
                   lv_image_decoder_dsc_t* dsc)
{
    if(dsc && dsc->decoded) {
        lv_draw_buf_destroy(const_cast<lv_draw_buf_t*>(dsc->decoded));
        dsc->decoded = nullptr;
    }
}

} // namespace

void raw_thumb_decoder_init()
{
    if(g_decoder) {
        return;
    }
    g_decoder = lv_image_decoder_create();
    if(!g_decoder) {
        return;
    }
    lv_image_decoder_set_info_cb(g_decoder, decoder_info);
    lv_image_decoder_set_open_cb(g_decoder, decoder_open);
    lv_image_decoder_set_close_cb(g_decoder, decoder_close);
    g_decoder->name = "RAW_THUMB";
}

} // namespace ui
