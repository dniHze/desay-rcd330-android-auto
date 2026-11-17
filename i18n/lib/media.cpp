// SPDX-License-Identifier: GPL-3.0-or-later
//
// The track's cover on the media player's USB screen, in place of its music note.
//
// Stock (mediaplayer, addresses per build in AA_MEDIA, from i18n/gen_media.py): the media service
// reads a file's tags, cover included (JPEG or PNG bytes), and the player keeps them through
// SVPPlayerState::SetID3(const ID3&): ID3 is {title, artist, album, ?, int size (+16), int format
// (+20), boost::shared_array<const unsigned char> cover (+24)}. SVPMusicPlayerMainDlg::UpdateID3
// shows the texts; the square on the right is the plane Allobject/Mat_Source_USB, whose texture
// (KZU_PROPERTY_TYPE_TEXTURE, a local property) is the note. The app never shows the cover.
//
// Here, in the media player only, when both functions have exactly the code of a known build (else
// the note stays), and not with the language "off" (the aa-lang-off card):
//   SetID3  wrapped: the cover's bytes (up to 4 MB, and up to 3000 x 3000 pixels by its header, as
//           it is decoded whole) go to AA_COVER_DIR/aa-cover.jpg or .png
//           (written to a temporary name, then renamed), or none; a counter tells the UI thread.
//           It may run outside the UI thread: only the file and two ints are shared.
//   UpdateID3 (USB)  wrapped, in the UI thread: after the stock code, a new cover is loaded with
//           Kanzi's own calls (kzcImageLoadFile), cropped to its centred square, resized to the
//           note's square and put where that square is on a transparent picture of the plane's
//           shape (on_canvas), made a texture (kzuImageTextureCreateFromImage, which keeps the
//           image) and set as the plane's texture; a track without one gets the note back.
//           References: setting a texture property releases the old one and acquires the new
//           one, so the note is acquired once
//           (it must outlive the time the cover replaces it) and only the latest cover is kept by
//           us; a screen made again (the plane has the note) gets the cover again.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "aa_lang.h"

#ifndef AA_COVER_DIR
#define AA_COVER_DIR "/tmp"
#endif

namespace {

struct MediaFunc {
    const char* name;
    uint32_t addr, size, fnv;
};
#ifdef AA_MEDIA
#include AA_MEDIA
#else
const MediaFunc kMediaFuncs[1] = {{"", 0, 0, 0}};
#endif

const int kMaxCover = 4 << 20;       // bytes of the file
const unsigned kMaxSide = 3000;      // pixels: decoded whole before the crop (3000 x 3000 RGB, 27 MB)

typedef void (*SetId3)(void*, const void*);
typedef void (*Method0)(void*);
SetId3 g_orig_set;
Method0 g_orig_usb;

volatile int g_gen;   // covers written (SetID3), and
volatile int g_kind;  // the latest one's: 0 none, 1 JPEG, 2 PNG
int g_done = -1;      // the generation the UI thread made a texture for
void* g_cover;        // that texture's resource (our reference), or 0
void* g_note;         // the plane's own texture (our reference), or 0

struct Kanzi {
    void* (*load_rel)(void*, const char*);  // SVPBaseDlg::LoadRelativeNode
    void** texture;                         // KZU_PROPERTY_TYPE_TEXTURE
    int (*get_res)(void*, void*, void**);
    int (*set_res)(void*, void*, void*);
    void (*acquire)(void*);
    void (*release)(void*);
    void* (*memory)(const void*);
    int (*load)(void*, const char*, void**);
    unsigned (*width)(void*);
    unsigned (*height)(void*);
    int (*crop)(void*, unsigned, unsigned, unsigned, unsigned);
    int (*resize)(void*, unsigned, unsigned, int);
    void (*image_delete)(void*);
    int (*format)(void*);
    void* (*data)(void*);
    int (*is_flipped)(void*);
    void (*set_flipped)(void*, int);
    int (*load_memory)(void*, unsigned, unsigned, int, const void*, void**);
    void* (*domain)(void*);
    void* (*resources)(void*);
    int (*make_texture)(void*, const char*, void*, int, int, float, void**);
    void* (*to_resource)(void*);
    void (*invalidate)(void*);
} g_kz;

uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

const char* cover_path(int kind) {
    return kind == 2 ? AA_COVER_DIR "/aa-cover.png" : AA_COVER_DIR "/aa-cover.jpg";
}

// The picture's size from its header: PNG's IHDR, or a JPEG's first SOFn (frame) marker. The file
// size says nothing about it: a small file can declare a picture that takes gigabytes to decode.
bool picture_size(const unsigned char* p, int n, int kind, unsigned& w, unsigned& h) {
    if (kind == 2) {
        if (n < 24 || memcmp(p + 12, "IHDR", 4) != 0)
            return false;
        w = (unsigned(p[16]) << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
        h = (unsigned(p[20]) << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
        return true;
    }
    for (int i = 2; i + 4 <= n;) {  // JPEG: markers FF xx, each but the standalone ones with a length
        if (p[i] != 0xFF)
            return false;
        int m = p[i + 1];
        if (m == 0xFF || m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {  // fill byte, TEM, RSTn, SOI
            i += m == 0xFF ? 1 : 2;
            continue;
        }
        if (m == 0xD9 || m == 0xDA)  // EOI or the scan before any frame
            return false;
        int len = (p[i + 2] << 8) | p[i + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {  // SOFn
            if (len < 7 || i + 9 > n)
                return false;
            h = (p[i + 5] << 8) | p[i + 6];
            w = (p[i + 7] << 8) | p[i + 8];
            return true;
        }
        if (len < 2)
            return false;
        i += 2 + len;
    }
    return false;
}

void hook_set_id3(void* self, const void* id3) {
    const char* p = static_cast<const char*>(id3);
    int size = *reinterpret_cast<const int*>(p + 16);
    const unsigned char* img = *reinterpret_cast<unsigned char* const*>(p + 24);
    int kind = 0;
    unsigned w = 0, h = 0;
    if (img && size > 8 && size <= kMaxCover) {
        kind = img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G' ? 2
               : img[0] == 0xFF && img[1] == 0xD8                                ? 1
                                                                                 : 0;
        if (kind && (!picture_size(img, size, kind, w, h) || !w || !h || w > kMaxSide || h > kMaxSide))
            kind = 0;  // a picture too large to decode here, or one whose size cannot be read: the note
        const char* tmp = AA_COVER_DIR "/aa-cover.tmp";
        FILE* f = kind ? fopen(tmp, "wb") : 0;
        bool ok = f && fwrite(img, 1, size, f) == static_cast<size_t>(size);
        if (f)
            ok = fclose(f) == 0 && ok;
        if (!ok || rename(tmp, cover_path(kind)) != 0)
            kind = 0;
    }
    g_kind = kind;
    __sync_fetch_and_add(&g_gen, 1);
    g_orig_set(self, id3);
}

// The cover laid out as the note's picture is: the plane is taller than the note's square (its
// picture has room for a reflection below), so the cover goes where the square is and the rest is
// transparent. Measured on the screen: plane 243 x 277 px, square at (3, 4), 238 x 235.
const unsigned kCanvasW = 256, kCanvasH = 292, kCoverX = 3, kCoverY = 4, kCoverW = 251, kCoverH = 248;
const int kRgb = 2, kRgba = 3;  // Kanzi's image data formats R8G8B8 and R8G8B8A8 (3 and 4 bytes)

// `image` (cropped and resized to kCoverW x kCoverH, RGB or RGBA) on a transparent RGBA canvas, as a
// new image (kzcImageLoadMemory copies the pixels), or 0.
void* on_canvas(void* kz, void* image) {
    int format = g_kz.format(image);
    const unsigned char* src = static_cast<const unsigned char*>(g_kz.data(image));
    if ((format != kRgb && format != kRgba) || !src || g_kz.width(image) != kCoverW || g_kz.height(image) != kCoverH)
        return 0;
    unsigned bpp = format == kRgba ? 4 : 3;
    unsigned char* canvas = static_cast<unsigned char*>(calloc(kCanvasW * kCanvasH, 4));
    if (!canvas)
        return 0;
    bool flipped = g_kz.is_flipped(image);  // rows stored bottom-up: the square is near the end
    unsigned top = flipped ? kCanvasH - kCoverY - kCoverH : kCoverY;
    for (unsigned y = 0; y < kCoverH; ++y)
        for (unsigned x = 0; x < kCoverW; ++x) {
            const unsigned char* s = src + (y * kCoverW + x) * bpp;
            unsigned char* d = canvas + ((top + y) * kCanvasW + kCoverX + x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = bpp == 4 ? s[3] : 255;
        }
    void* out = 0;
    if (g_kz.load_memory(g_kz.memory(kz), kCanvasW, kCanvasH, kRgba, canvas, &out) != 0)
        out = 0;
    free(canvas);
    if (out)
        g_kz.set_flipped(out, flipped);
    return out;
}

// The latest cover as a texture resource (our reference), or 0.
void* make_cover(void* kz, int kind) {
    void* image = 0;
    if (g_kz.load(g_kz.memory(kz), cover_path(kind), &image) != 0 || !image)
        return 0;
    unsigned w = g_kz.width(image), h = g_kz.height(image), s = w < h ? w : h;
    void* canvas = 0;
    if (s && g_kz.crop(image, (w - s) / 2, (h - s) / 2, s, s) == 0 && g_kz.resize(image, kCoverW, kCoverH, 1) == 0)
        canvas = on_canvas(kz, image);
    g_kz.image_delete(image);
    if (!canvas)
        return 0;
    void* texture = 0;
    static unsigned n;
    char name[24];
    snprintf(name, sizeof name, "aa-cover-%u", ++n);
    if (g_kz.make_texture(g_kz.resources(g_kz.domain(kz)), name, canvas, 2, 2, 0.0f, &texture) != 0 || !texture) {
        g_kz.image_delete(canvas);  // the texture keeps its image; without one it is ours
        return 0;
    }
    return g_kz.to_resource(texture);
}

void hook_usb_id3(void* self) {
    g_orig_usb(self);
    void* node = g_kz.load_rel(self, "Allobject/Mat_Source_USB");
    void* kz = node ? *reinterpret_cast<void**>(static_cast<char*>(node) + 0x50) : 0;
    void* now = 0;
    if (!kz || !g_kz.get_res(kz, *g_kz.texture, &now) || !now)
        return;
    if (!g_note && now != g_cover) {  // the plane's own picture: kept, for a track without a cover
        g_note = now;
        g_kz.acquire(g_note);
    }
    int gen = g_gen, kind = g_kind;
    if (gen != g_done) {
        g_done = gen;
        void* cover = kind ? make_cover(kz, kind) : 0;
        if (g_cover)
            g_kz.release(g_cover);  // the plane may still hold it, until it is replaced below
        g_cover = cover;
        if (kind && !cover)
            syslog(LOG_WARNING, "aa-media: the cover could not be loaded; the note stays");
    }
    void* want = g_cover ? g_cover : g_note;
    if (want && want != now) {
        g_kz.set_res(kz, *g_kz.texture, want);
        g_kz.invalidate(kz);
    }
}

bool is_media_player() {
    char exe[128];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    return strstr(exe, "/sv.g5.mib2.mediaplayer/bin/mediaplayer") != 0;
}

}  // namespace

void aa_media_init() {
    if (!is_media_player())
        return;
    for (size_t i = 0; i < sizeof kMediaFuncs / sizeof kMediaFuncs[0]; ++i) {
        const MediaFunc& f = kMediaFuncs[i];
        if (!f.addr || fnv(reinterpret_cast<const uint8_t*>(f.addr), f.size) != f.fnv) {
            syslog(LOG_WARNING, "aa-media: %s is not the known code; no covers", f.name);
            return;
        }
    }
#define KZ(f, name) g_kz.f = reinterpret_cast<decltype(g_kz.f)>(dlsym(RTLD_DEFAULT, name))
    KZ(load_rel, "_ZN10SVPBaseDlg16LoadRelativeNodeEPKc");
    KZ(texture, "KZU_PROPERTY_TYPE_TEXTURE");
    KZ(get_res, "kzuObjectNodeGetResourceIDResourceProperty");
    KZ(set_res, "kzuObjectNodeSetResourceIDResourceProperty");
    KZ(acquire, "kzuResourceAcquire");
    KZ(release, "kzuResourceRelease");
    KZ(memory, "kzcMemoryGetManager");
    KZ(load, "kzcImageLoadFile");
    KZ(width, "kzcImageGetWidth");
    KZ(height, "kzcImageGetHeight");
    KZ(crop, "kzcImageCrop");
    KZ(resize, "kzcImageResize");
    KZ(image_delete, "kzcImageDelete");
    KZ(format, "kzcImageGetDataFormat");
    KZ(data, "kzcImageGetData");
    KZ(is_flipped, "kzcImageIsFlippedInternally");
    KZ(set_flipped, "kzcImageSetFlippedInternally");
    KZ(load_memory, "kzcImageLoadMemory");
    KZ(domain, "kzuObjectNodeGetUIDomain");
    KZ(resources, "kzuUIDomainGetResourceManager");
    KZ(make_texture, "kzuImageTextureCreateFromImage");
    KZ(to_resource, "kzuImageTextureToResource");
    KZ(invalidate, "kzuObjectNodeInvalidateRender");
#undef KZ
    void** all = reinterpret_cast<void**>(&g_kz);
    for (size_t i = 0; i < sizeof g_kz / sizeof(void*); ++i)
        if (!all[i]) {
            syslog(LOG_WARNING, "aa-media: a Kanzi function is missing; no covers");
            return;
        }
    uint32_t set = kMediaFuncs[0].addr, usb = kMediaFuncs[1].addr;  // gen_media.py's order
    g_orig_set = reinterpret_cast<SetId3>(aa_hook_trampoline(set));
    g_orig_usb = reinterpret_cast<Method0>(aa_hook_trampoline(usb));
    if (!g_orig_set || !g_orig_usb || !aa_hook_jump(usb, reinterpret_cast<const void*>(hook_usb_id3)) ||
        !aa_hook_jump(set, reinterpret_cast<const void*>(hook_set_id3))) {
        syslog(LOG_ERR, "aa-media: patching failed");
        return;
    }
    syslog(LOG_INFO, "aa-media: covers on the USB screen");
}
