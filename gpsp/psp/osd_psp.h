/* osd_psp.h — overlay/OSD framework (plan §8): toast queue (top-center),
 * persistent session chip (bottom-right) and FF chip (top-right).
 * osd_draw() renders one overlay pass; call once per presented frame,
 * after the game blit and before vid_swap(). All main-thread. */
#ifndef OSD_PSP_H
#define OSD_PSP_H

#ifdef __cplusplus
extern "C" {
#endif

void osd_toast(const char *fmt, ...);
void osd_chip_session(const char *text);  /* NULL/"" hides the chip */
void osd_chip_ff(const char *text);       /* NULL/"" hides the chip */
void osd_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* OSD_PSP_H */
