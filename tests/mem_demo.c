/*
 * mem_demo.c -- a deliberately simple, pointer-level demonstration of paging
 * and FIFO page replacement (Task 2, the "literal" version).
 *
 * This complements the full buffer pool (src/bufferpool.c). Here everything is
 * explicit and visible:
 *   - physical memory is a small array of frames, each an int (4 bytes);
 *   - the workload is a "reference string" -- an array of page numbers;
 *   - pages are read/written THROUGH POINTERS and dereferenced;
 *   - when memory is full, FIFO chooses the victim;
 *   - the buffer (frame contents) is printed after every reference.
 *
 * It prints page faults vs hits and the final hit ratio, so the mechanics of
 * paging are laid bare for the report.
 */
#include <stdio.h>
#include <stddef.h>

#define NFRAMES 4      /* physical memory: 4 frames                       */

int main(void)
{
    /* physical memory -- an empty array of frames (each frame is 4 bytes) */
    int  frames[NFRAMES];
    int  valid[NFRAMES] = {0};        /* is this frame currently loaded?   */
    int  next_victim = 0;             /* FIFO pointer: oldest-loaded frame  */

    /* the reference string: the sequence of page numbers requested */
    int reference[] = { 1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5 };
    size_t nref = sizeof(reference) / sizeof(reference[0]);

    printf("frame size = %zu bytes, %d frames, reference string of %zu pages\n\n",
           sizeof(int), NFRAMES, nref);
    printf("ref  result   frames [f0 f1 f2 f3]\n");
    printf("---  -------   ---------------------\n");

    int faults = 0, hits = 0;

    for (size_t i = 0; i < nref; i++) {
        int page = reference[i];

        /* --- is the page already resident? (search the frames) --- */
        int hit_frame = -1;
        for (int f = 0; f < NFRAMES; f++) {
            int *slot = &frames[f];          /* pointer into physical memory */
            if (valid[f] && *slot == page) { /* dereference to compare      */
                hit_frame = f;
                break;
            }
        }

        const char *result;
        if (hit_frame >= 0) {
            result = "HIT  ";
            hits++;
        } else {
            /* page fault: find a free frame, else evict the FIFO victim */
            int target = -1;
            for (int f = 0; f < NFRAMES; f++)
                if (!valid[f]) { target = f; break; }

            if (target < 0) {                 /* memory full -> FIFO victim  */
                target = next_victim;
                next_victim = (next_victim + 1) % NFRAMES;
            } else {
                /* still advance the FIFO pointer past freshly filled frames */
                next_victim = (target + 1) % NFRAMES;
            }

            int *slot = &frames[target];      /* pointer to the chosen frame */
            *slot = page;                     /* dereference + store the page */
            valid[target] = 1;
            result = "FAULT";
            faults++;
        }

        /* --- print the buffer (physical memory) after this reference --- */
        printf(" %d   %s    [", page, result);
        for (int f = 0; f < NFRAMES; f++) {
            if (valid[f]) printf("%2d", frames[f]); else printf(" .");
            printf(f == NFRAMES - 1 ? "]" : " ");
        }
        printf("\n");
    }

    printf("\nfaults = %d, hits = %d, hit ratio = %.1f%%\n",
           faults, hits, 100.0 * hits / (double)nref);
    return 0;
}
