/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * va_barcode_test.c — test de corrección frame-a-frame para rockchip-vaapi
 *
 * Decodifica un IVF/VP9 generado con gen_test_video.py y verifica que cada
 * frame mostrado contiene el código de barras correcto (número de frame en
 * orden de display).  Detecta frames con contenido incorrecto, frames en
 * blanco/negro (sync falla) y errores de exportación.
 *
 * Barcode (debe coincidir con gen_test_video.py):
 *   - Banda izquierda: BARCODE_W píxeles de ancho
 *   - N_BANDS = 13 bandas horizontales:
 *       bandas 0,1,2 = sync (blanco/negro/blanco)
 *       bandas 3..12 = 10 bits del número de frame MSB primero
 *   - Y = 235 → 1 (blanco),  Y = 16 → 0 (negro)
 *
 * Build:
 *   gcc -O1 -o va_barcode_test src/va_barcode_test.c \
 *       -lva -lva-drm -ldrm -I/usr/include/libdrm
 *
 * Usage:
 *   LIBVA_DRIVER_NAME=rockchip ./va_barcode_test video.ivf [video2.ivf ...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

/* ── Constantes del barcode (deben coincidir con gen_test_video.py) ─────── */

#define BARCODE_W   64      /* ancho de la banda de barcode en píxeles */
#define N_BANDS     13      /* 3 sync + 10 datos */

/* ── IVF format ────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  sig[4];
    uint16_t version;
    uint16_t hdr_len;
    uint32_t fourcc;
    uint16_t width;
    uint16_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t nframes;
    uint32_t rsvd;
} IvfFileHeader;

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t pts;
} IvfFrameHeader;

/* ── VP9 bitstream helpers ─────────────────────────────────────────────── */

static int vp9_show_frame(const uint8_t *d, size_t len)
{
    if (!d || len < 1) return 1;
    uint8_t b = d[0];
    if ((b >> 6) != 2) return 1;
    int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
    if (profile != 3) {
        if ((b >> 3) & 1) return 1;
        return (b >> 1) & 1;
    } else {
        if ((b >> 2) & 1) return 1;
        return b & 1;
    }
}

static int vp9_is_keyframe(const uint8_t *d, size_t len)
{
    if (!d || len < 1) return 1;
    uint8_t b = d[0];
    if ((b >> 6) != 2) return 1;
    int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
    if (profile != 3) {
        if ((b >> 3) & 1) return 0;
        return !((b >> 2) & 1);
    } else {
        if ((b >> 2) & 1) return 0;
        return !(b & 1);
    }
}

/* ── Lectura del barcode ────────────────────────────────────────────────── */

/*
 * decode_barcode — lee el barcode de la banda izquierda del plano Y.
 *
 * @y      puntero al inicio del plano Y (offset 0 del mmap)
 * @stride stride del plano Y en bytes
 * @h      altura del frame en píxeles
 *
 * Retorna: número de frame codificado (0..1023) o
 *          -1 si el patrón de sincronización no se reconoce
 *          -2 si la muestra está fuera del rango esperado (frame corrupto)
 *
 * Estrategia: para cada banda, promedia los píxeles del cuarto central de
 * la banda (filas centrales) y de x = BARCODE_W/4 a 3*BARCODE_W/4.
 * Umbral: Y > 128 → bit=1, Y ≤ 128 → bit=0.
 */
static int decode_barcode(const uint8_t *y, int stride, int h)
{
    int band_h = h / N_BANDS;
    if (band_h < 4) return -2;   /* resolución demasiado pequeña */

    int avg[N_BANDS];
    int x0 = BARCODE_W / 4;
    int x1 = 3 * BARCODE_W / 4;
    if (x1 <= x0) { x0 = 0; x1 = BARCODE_W; }

    for (int b = 0; b < N_BANDS; b++) {
        int r_base = b * band_h;
        int r0 = r_base + band_h / 4;
        int r1 = r_base + 3 * band_h / 4;
        if (b == N_BANDS - 1) r1 = h - band_h / 8;
        if (r1 >= h) r1 = h - 1;
        if (r0 >= r1) r0 = r_base;

        long sum = 0;
        int  cnt = 0;
        for (int r = r0; r < r1; r++)
            for (int x = x0; x < x1; x++) {
                sum += y[(size_t)r * stride + x];
                cnt++;
            }
        avg[b] = cnt > 0 ? (int)(sum / cnt) : 128;
    }

    /* Verificar sync: bandas 0,1,2 deben ser 1,0,1 */
    int s0 = avg[0] > 128;
    int s1 = avg[1] > 128;
    int s2 = avg[2] > 128;
    if (s0 != 1 || s1 != 0 || s2 != 1)
        return -1;   /* sync fail */

    /* Leer 10 bits de datos */
    int val = 0;
    for (int b = 3; b < N_BANDS; b++)
        val = (val << 1) | (avg[b] > 128 ? 1 : 0);

    return val;
}

/* ── VA-API helpers ─────────────────────────────────────────────────────── */

#define N_SURFACES 16

#define CHECK_VA(call) do {                                             \
    VAStatus _s = (call);                                               \
    if (_s != VA_STATUS_SUCCESS) {                                      \
        fprintf(stderr, "VA error %d at %s:%d: %s\n",                  \
                _s, __FILE__, __LINE__, vaErrorStr(_s));                \
        return 1;                                                       \
    }                                                                   \
} while (0)

/* ── Test de un IVF ─────────────────────────────────────────────────────── */

static int test_ivf(const char *path, VADisplay dpy)
{
    FILE *ivf = fopen(path, "rb");
    if (!ivf) { perror(path); return 1; }

    IvfFileHeader fh;
    if (fread(&fh, sizeof(fh), 1, ivf) != 1 ||
        memcmp(fh.sig, "DKIF", 4) != 0) {
        fprintf(stderr, "%s: not an IVF file\n", path);
        fclose(ivf);
        return 1;
    }
    int W = fh.width, H = fh.height, NF = fh.nframes;
    printf("\n=== %s  %dx%d  %d frames  codec=%c%c%c%c ===\n",
           path, W, H, NF,
           (char)(fh.fourcc), (char)(fh.fourcc>>8),
           (char)(fh.fourcc>>16), (char)(fh.fourcc>>24));

    /* Config + surfaces + context */
    VAConfigID cfg = VA_INVALID_ID;
    CHECK_VA(vaCreateConfig(dpy, VAProfileVP9Profile0,
                            VAEntrypointVLD, NULL, 0, &cfg));

    VASurfaceID surfs[N_SURFACES];
    {
        VASurfaceAttrib att[2] = {
            { VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_SETTABLE,
              { VAGenericValueTypeInteger, { .i = VA_FOURCC_NV12 } } },
            { VASurfaceAttribUsageHint,   VA_SURFACE_ATTRIB_SETTABLE,
              { VAGenericValueTypeInteger,
                { .i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER } } }
        };
        CHECK_VA(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420,
                                  W, H, surfs, N_SURFACES, att, 2));
    }
    VAContextID ctx = VA_INVALID_ID;
    CHECK_VA(vaCreateContext(dpy, cfg, W, H,
                             VA_PROGRESSIVE, surfs, N_SURFACES, &ctx));

    /* Decode loop */
    uint8_t *frame_buf = NULL;
    size_t   frame_cap = 0;
    int surf_idx = 0;
    int display_counter = 0;   /* frames mostrados (mostrados, no altrefs) */
    int errors = 0, sync_fails = 0, export_fails = 0;
    int total_shown = 0, total_hidden = 0;

    /* Tabla de anomalías para resumen */
    typedef struct { int ivf_fn; int disp_fn; int expected; int actual; char type; } Anomaly;
    Anomaly anomalies[256];
    int n_anomalies = 0;

    for (int fn = 0; fn < NF; fn++) {
        IvfFrameHeader frhdr;
        if (fread(&frhdr, sizeof(frhdr), 1, ivf) != 1) {
            fprintf(stderr, "Truncated at frame %d\n", fn);
            break;
        }
        uint32_t sz = frhdr.size;
        if (sz > frame_cap) {
            frame_buf = realloc(frame_buf, sz);
            frame_cap = sz;
        }
        if (fread(frame_buf, 1, sz, ivf) != sz) {
            fprintf(stderr, "Truncated frame %d\n", fn);
            break;
        }

        int shown  = vp9_show_frame(frame_buf, sz);
        int is_key = vp9_is_keyframe(frame_buf, sz);

        VASurfaceID surf = surfs[surf_idx % N_SURFACES];
        surf_idx++;

        /* Decode */
        CHECK_VA(vaBeginPicture(dpy, ctx, surf));
        VABufferID slice_buf = VA_INVALID_ID;
        CHECK_VA(vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
                                sz, 1, frame_buf, &slice_buf));
        CHECK_VA(vaRenderPicture(dpy, ctx, &slice_buf, 1));
        CHECK_VA(vaEndPicture(dpy, ctx));
        CHECK_VA(vaDestroyBuffer(dpy, slice_buf));
        CHECK_VA(vaSyncSurface(dpy, surf));

        /* Clasificación del frame */
        const char *ftype = is_key ? "KEY" : (shown ? "   " : "ALT");

        if (!shown) {
            /* Altref: mostrado como show_frame=0 en el bitstream.
             * Firefox llama BeginPicture/EndPicture para estos frames y
             * luego puede llamar ExportSurfaceHandle si el compositor
             * intenta mostrar la superficie.  Probamos el export aquí. */
            total_hidden++;
            VADRMPRIMESurfaceDescriptor desc2 = {0};
            VAStatus est2 = vaExportSurfaceHandle(
                dpy, surf,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                &desc2);
            int alt_barcode = -999;
            const char *alt_res = "EXPORT_FAIL";
            if (est2 == VA_STATUS_SUCCESS) {
                int exp_fd2   = desc2.objects[0].fd;
                int stride2   = (int)desc2.layers[0].pitch[0];
                int y_off2    = (int)desc2.layers[0].offset[0];
                int fr_h2     = (int)desc2.height;
                size_t map_sz2 = (size_t)stride2 * fr_h2 + (size_t)y_off2 + 4096;
                void *ptr2 = mmap(NULL, map_sz2, PROT_READ, MAP_SHARED, exp_fd2, 0);
                if (ptr2 != MAP_FAILED) {
                    alt_barcode = decode_barcode((const uint8_t *)ptr2 + y_off2,
                                                 stride2, fr_h2);
                    munmap(ptr2, map_sz2);
                    alt_res = alt_barcode >= 0 ? "barcode_ok" : "no_sync";
                } else {
                    alt_res = "MMAP_FAIL";
                }
                close(exp_fd2);
            }
            printf("ivf %4d [%s hiddn] sz=%7u  export=%s barcode=%d\n",
                   fn, ftype, sz, alt_res, alt_barcode);
            fflush(stdout);
            continue;
        }

        total_shown++;
        /* ffmpeg outputs IVF packets in source order: IVF position fn = source frame fn */
        int expected = fn;
        display_counter++;

        /* Exportar DMA-BUF y leer barcode */
        VADRMPRIMESurfaceDescriptor desc = {0};
        VAStatus est = vaExportSurfaceHandle(
            dpy, surf,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &desc);

        int actual_barcode = -999;
        const char *result_str = "EXPORT_FAIL";
        int ok = 0;

        if (est != VA_STATUS_SUCCESS) {
            export_fails++;
            result_str = "EXPORT_FAIL";
            if (n_anomalies < 256) {
                anomalies[n_anomalies++] = (Anomaly){fn, expected, expected, -999, 'E'};
            }
        } else {
            /* Mapear plano Y */
            int exp_fd    = desc.objects[0].fd;
            int stride    = (int)desc.layers[0].pitch[0];
            int y_offset  = (int)desc.layers[0].offset[0];
            int fr_h      = (int)desc.height;
            size_t map_sz = (size_t)stride * fr_h + (size_t)y_offset + 4096;

            void *ptr = mmap(NULL, map_sz, PROT_READ, MAP_SHARED, exp_fd, 0);
            if (ptr == MAP_FAILED) {
                result_str = "MMAP_FAIL";
                export_fails++;
            } else {
                const uint8_t *yplane = (const uint8_t *)ptr + y_offset;
                actual_barcode = decode_barcode(yplane, stride, fr_h);
                munmap(ptr, map_sz);

                if (actual_barcode < 0) {
                    sync_fails++;
                    result_str = actual_barcode == -1 ? "NO_SYNC" : "TOO_SMALL";
                    if (n_anomalies < 256)
                        anomalies[n_anomalies++] = (Anomaly){fn, expected, expected, actual_barcode, 'S'};
                } else if (actual_barcode == expected) {
                    result_str = "OK";
                    ok = 1;
                } else {
                    errors++;
                    result_str = "WRONG";
                    if (n_anomalies < 256)
                        anomalies[n_anomalies++] = (Anomaly){fn, expected, expected, actual_barcode, 'W'};
                }
            }
            close(exp_fd);
        }

        printf("ivf %4d [%s shown] disp=%4d  exp=%4d  got=%4d  sz=%7u  %s%s\n",
               fn, ftype, expected, expected, actual_barcode, sz,
               ok ? "OK" : result_str,
               ok ? "" : " ***");
        fflush(stdout);
    }

    /* Resumen */
    int total_errors = errors + sync_fails + export_fails;
    printf("\n--- %s ---\n", path);
    printf("  IVF frames : %d  (shown=%d hidden=%d)\n", NF, total_shown, total_hidden);
    printf("  WRONG      : %d\n", errors);
    printf("  SYNC_FAIL  : %d\n", sync_fails);
    printf("  EXPORT_FAIL: %d\n", export_fails);
    printf("  RESULT     : %s\n\n", total_errors == 0 ? "PASS" : "FAIL");

    if (n_anomalies > 0) {
        printf("Anomalías:\n");
        for (int i = 0; i < n_anomalies; i++) {
            Anomaly *a = &anomalies[i];
            if (a->type == 'E')
                printf("  ivf=%4d disp=%4d  EXPORT_FAIL\n", a->ivf_fn, a->disp_fn);
            else if (a->type == 'S')
                printf("  ivf=%4d disp=%4d  NO_SYNC (barcode=%d)\n",
                       a->ivf_fn, a->disp_fn, a->actual);
            else
                printf("  ivf=%4d disp=%4d  exp=%4d got=%4d  delta=%+d\n",
                       a->ivf_fn, a->disp_fn, a->expected, a->actual,
                       a->actual - a->expected);
        }
        putchar('\n');
    }

    free(frame_buf);
    vaDestroyContext(dpy, ctx);
    for (int i = 0; i < N_SURFACES; i++)
        vaDestroySurfaces(dpy, &surfs[i], 1);
    vaDestroyConfig(dpy, cfg);
    fclose(ivf);

    return total_errors > 0 ? 1 : 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s video.ivf [video2.ivf ...]\n", argv[0]);
        return 1;
    }

    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { perror("/dev/dri/renderD128"); return 1; }

    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 1; }

    int major, minor;
    VAStatus vs = vaInitialize(dpy, &major, &minor);
    if (vs != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize: %s\n", vaErrorStr(vs));
        return 1;
    }
    printf("VA-API %d.%d  driver: %s\n", major, minor, vaQueryVendorString(dpy));

    int result = 0;
    for (int i = 1; i < argc; i++)
        result |= test_ivf(argv[i], dpy);

    vaTerminate(dpy);
    close(drm_fd);
    return result;
}
