#pragma once

// The extractor yields to FreeRTOS between pages; on the host that is a no-op.
inline void vTaskDelay(int) {}
