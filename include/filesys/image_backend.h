#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void image_backend_init(const char* path);
void image_backend_shutdown(void);
int  image_backend_is_ready(void);

#ifdef __cplusplus
}
#endif
