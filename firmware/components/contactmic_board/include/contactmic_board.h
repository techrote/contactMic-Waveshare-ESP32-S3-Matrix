#ifndef CONTACTMIC_BOARD_H
#define CONTACTMIC_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Accepted Waveshare schematic: docs/rag/02-HARDWARE-CONTRACT.md. */
#define CMQ_SDA_GPIO 11
#define CMQ_SCL_GPIO 12
#define CMQ_INT1_GPIO 10
#define CMQ_INT2_GPIO 13
#define CMQ_I2C_MAX_HZ 400000u

typedef enum { CMQ_OK = 0, CMQ_INVALID, CMQ_STATE, CMQ_NOT_FOUND,
               CMQ_IO, CMQ_IDENTITY, CMQ_AMBIGUOUS, CMQ_VERIFY } cmq_result;
typedef enum { CMQ_BUS_OK = 0, CMQ_BUS_ABSENT, CMQ_BUS_ERROR } cmq_bus_result;
typedef enum { CMQ_X = 0, CMQ_Y = 1, CMQ_Z = 2 } cmq_axis;
typedef enum { CMQ_SAFE = 0, CMQ_MEDIUM = 1, CMQ_HIGH = 2 } cmq_mode;
typedef enum { CMQ_UNPROBED = 0, CMQ_IDENTIFIED, CMQ_IDLE,
               CMQ_CONFIGURED, CMQ_RUNNING, CMQ_FAULT } cmq_state;

typedef struct {
    void *context;
    cmq_bus_result (*probe)(void *, uint8_t address);
    cmq_bus_result (*read)(void *, uint8_t address, uint8_t reg, uint8_t *, size_t);
    cmq_bus_result (*write)(void *, uint8_t address, uint8_t reg, uint8_t value);
    void (*delay_us)(void *, uint32_t microseconds);
} cmq_bus;
typedef struct {
    cmq_axis axis;
    cmq_mode mode;
    uint8_t range_g; /* 2 or 4; gyro always enabled in these 6DOF modes. */
    bool drdy_enabled;
} cmq_config;
typedef struct {
    uint64_t probe_attempts, probe_misses, identity_rejections;
    uint64_t reads, writes, i2c_errors, verification_errors, samples_read;
} cmq_counters;
typedef struct {
    cmq_bus bus;
    cmq_state state;
    cmq_config config;
    cmq_counters counters;
    uint8_t address, who_am_i, revision;
} cmq_device;

/* Single-owner task API: never call from an ISR or concurrently. No BLE dependency.
 * An initialized device must not be reinitialized while its sensor is running.
 * All configuration I/O failures latch FAULT; reset is required before start.
 * Failed sample reads leave the output unchanged and remain recoverable per sample.
 */
cmq_result cmq_init(cmq_device *, const cmq_bus *, uint32_t bus_hz);
cmq_result cmq_probe(cmq_device *);
cmq_result cmq_reset(cmq_device *);
cmq_result cmq_configure(cmq_device *, const cmq_config *);
cmq_result cmq_start(cmq_device *);
cmq_result cmq_stop(cmq_device *);
cmq_result cmq_set_drdy(cmq_device *, bool enabled);
cmq_result cmq_read_axis(cmq_device *, cmq_axis, int16_t *sample);
cmq_result cmq_read_selected(cmq_device *, int16_t *sample);
/* Read-only failure diagnosis, not acquired samples. Bytes: CTRL1/2/3/7/8,
 * FIFO_CTRL, STATUSINT/0, timestamp low/mid, selected axis low/high. Timestamp
 * bytes are sequential observations and can tear; never a source denominator. */
cmq_result cmq_diagnostic_snapshot(cmq_device *, uint8_t out[12]);
uint32_t cmq_nominal_rate_millihz(cmq_mode);
uint32_t cmq_settling_us(cmq_mode);

#ifdef __cplusplus
}
#endif
#endif
