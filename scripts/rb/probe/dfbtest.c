/* dfbtest.c - minimal DirectFB bring-up probe (soft-float, glibc 2.13).
 * Isolates the display stack from rbp: init, create primary surface, flip.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <directfb.h>

static int fail(const char *what, DFBResult r)
{
    printf("FAIL %-40s -> %d\n", what, r);
    fflush(stdout);
    return 1;
}
#define TRY(x) do { DFBResult _r = (x); printf("OK   %s\n", #x); fflush(stdout); if (_r) return fail(#x, _r); } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("dfbtest start pid=%d\n", (int)getpid());

    TRY(DirectFBInit(NULL, NULL));
    IDirectFB *dfb = NULL;
    TRY(DirectFBCreate(&dfb));

    /* Fullscreen cooperative level: disables the window manager so the
     * DSCAPS_PRIMARY surface IS the layer surface (which is what rbp does).
     * Otherwise the primary is a WM window in system memory and the layer
     * surface stays empty. */
    TRY(dfb->SetCooperativeLevel(dfb, DFSCL_FULLSCREEN));

    DFBSurfaceDescription dsc;
    memset(&dsc, 0, sizeof(dsc));
    dsc.flags = DSDESC_CAPS;
    dsc.caps  = DSCAPS_PRIMARY | DSCAPS_FLIPPING;

    IDirectFBSurface *prim = NULL;
    TRY(dfb->CreateSurface(dfb, &dsc, &prim));

    int w = 0, h = 0;
    TRY(prim->GetSize(prim, &w, &h));
    printf("primary surface: %dx%d\n", w, h);

    TRY(prim->Clear(prim, 0, 0, 0, 0xff));
    TRY(prim->Flip(prim, NULL, DSFLIP_WAITFORSYNC));
    printf("flipped BLACK; sleeping 3s\n"); fflush(stdout);
    sleep(3);

    TRY(prim->Clear(prim, 0xff, 0, 0, 0xff));
    TRY(prim->Flip(prim, NULL, DSFLIP_WAITFORSYNC));
    printf("flipped RED; sleeping 3s\n"); fflush(stdout);
    sleep(3);

    printf("dfbtest DONE\n");
    return 0;
}
