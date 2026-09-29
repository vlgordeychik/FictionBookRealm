#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void sd_backend_init(void);
void sd_backend_shutdown(void);
int  sd_backend_is_ready(void);

#ifdef __cplusplus
}
#endif
