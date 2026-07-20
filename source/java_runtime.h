/*
 * Shared runtime values exported from java.c.
 */

#ifndef MCSM_JAVA_RUNTIME_H
#define MCSM_JAVA_RUNTIME_H

int mcsm_get_framebuffer_width(void);
int mcsm_get_framebuffer_height(void);
int mcsm_get_render_scale_width(void);
int mcsm_get_render_scale_height(void);

#endif
