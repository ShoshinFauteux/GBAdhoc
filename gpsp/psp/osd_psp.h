/* osd_psp.h — overlay/OSD framework (plan §8): toast queue (top-center),
 * persistent session chip (bottom-right), FF chip (top-right), FPS chip
 * (top-left) and the link-wait chip (bottom-left).
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
void osd_chip_fps(const char *text);      /* top-left; NULL/"" hides */
/* Bottom-left, while a link session holds the game for the partner
 * (fe_host_link_wait).  The text is not copied: a string constant.
 * NULL hides the chip. */
void osd_chip_wait(const char *text);
void osd_draw(void);
/* Forget every toast and chip: the in-process return to the game list
 * starts the next game with the OSD a fresh boot has. */
void osd_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OSD_PSP_H */
