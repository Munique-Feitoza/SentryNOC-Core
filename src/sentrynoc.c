#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROTOCOL_VERSION 1
#define SAMPLE_BUFFER_SIZE 256
#define SAMPLE_LINE_SIZE 2048
#define DEFAULT_INTERVAL_MS 1000
#define DEFAULT_SPOOL_PATH "sentrynoc.spool"

typedef struct { uint64_t user, nice, system, idle, iowait, irq, softirq, steal; } cpu_stats_t;
typedef struct { uint64_t mem_total_kb, mem_available_kb, swap_total_kb, swap_free_kb; } memory_stats_t;
typedef struct { double some_avg10, full_avg10; uint64_t some_total_us, full_total_us; } psi_stats_t;
typedef struct { uint64_t read_ops, read_sectors, write_ops, write_sectors, io_ms; } disk_stats_t;
typedef struct { uint64_t rx_bytes, rx_packets, rx_errors, rx_drops, tx_bytes, tx_packets, tx_errors, tx_drops; } network_stats_t;

typedef struct {
    uint64_t sequence;
    int64_t timestamp_ms;
    cpu_stats_t cpu;
    memory_stats_t memory;
    psi_stats_t memory_psi;
    psi_stats_t io_psi;
    disk_stats_t disk;
    network_stats_t network;
} sample_t;

typedef struct { char lines[SAMPLE_BUFFER_SIZE][SAMPLE_LINE_SIZE]; size_t head, count; } sample_buffer_t;
static volatile sig_atomic_t running = 1;

static void stop_agent(int signal_number) { (void)signal_number; running = 0; }

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static int read_cpu_stats(cpu_stats_t *stats) {
    FILE *file = fopen("/proc/stat", "r"); char line[256]; int parsed;
    if (file == NULL || fgets(line, sizeof(line), file) == NULL) { if (file) fclose(file); return -1; }
    fclose(file);
    parsed = sscanf(line, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                    " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                    &stats->user, &stats->nice, &stats->system, &stats->idle,
                    &stats->iowait, &stats->irq, &stats->softirq, &stats->steal);
    return parsed >= 4 ? 0 : -1;
}

static int read_memory_stats(memory_stats_t *stats) {
    FILE *file = fopen("/proc/meminfo", "r"); char key[32], unit[16]; uint64_t value;
    if (file == NULL) return -1;
    memset(stats, 0, sizeof(*stats));
    while (fscanf(file, "%31[^:]: %" SCNu64 " %15s\n", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal") == 0) stats->mem_total_kb = value;
        else if (strcmp(key, "MemAvailable") == 0) stats->mem_available_kb = value;
        else if (strcmp(key, "SwapTotal") == 0) stats->swap_total_kb = value;
        else if (strcmp(key, "SwapFree") == 0) stats->swap_free_kb = value;
    }
    fclose(file); return stats->mem_total_kb > 0 ? 0 : -1;
}

static int read_psi(const char *path, psi_stats_t *stats) {
    FILE *file = fopen(path, "r"); char line[256], state[16];
    if (file == NULL) return -1;
    memset(stats, 0, sizeof(*stats));
    while (fgets(line, sizeof(line), file) != NULL) {
        double avg10, avg60, avg300; uint64_t total;
        if (sscanf(line, "%15s avg10=%lf avg60=%lf avg300=%lf total=%" SCNu64,
                   state, &avg10, &avg60, &avg300, &total) != 5) continue;
        if (strcmp(state, "some") == 0) { stats->some_avg10 = avg10; stats->some_total_us = total; }
        else if (strcmp(state, "full") == 0) { stats->full_avg10 = avg10; stats->full_total_us = total; }
    }
    fclose(file); return 0;
}

static int is_block_device(const char *name) {
    return strncmp(name, "sd", 2) == 0 || strncmp(name, "vd", 2) == 0 ||
           strncmp(name, "xvd", 3) == 0 || strncmp(name, "nvme", 4) == 0 ||
           strncmp(name, "mmcblk", 6) == 0;
}

static int read_disk_stats(disk_stats_t *stats) {
    FILE *file = fopen("/proc/diskstats", "r"); char name[64], line[256];
    unsigned int major, minor; uint64_t reads, merged_reads, sectors_read, read_ms;
    uint64_t writes, merged_writes, sectors_written, write_ms, in_progress, io_ms, weighted_ms;
    if (file == NULL) return -1;
    memset(stats, 0, sizeof(*stats));
    while (fgets(line, sizeof(line), file) != NULL) {
        int parsed = sscanf(line, "%u %u %63s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                            " %" SCNu64 " %" SCNu64, &major, &minor, name, &reads, &merged_reads,
                            &sectors_read, &read_ms, &writes, &merged_writes, &sectors_written,
                            &write_ms, &in_progress, &io_ms, &weighted_ms);
        (void)major; (void)minor; (void)merged_reads; (void)read_ms; (void)merged_writes;
        (void)write_ms; (void)in_progress; (void)weighted_ms;
        if (parsed == 14 && is_block_device(name)) {
            stats->read_ops += reads; stats->read_sectors += sectors_read;
            stats->write_ops += writes; stats->write_sectors += sectors_written; stats->io_ms += io_ms;
        }
    }
    fclose(file); return 0;
}

static int read_network_stats(network_stats_t *stats) {
    FILE *file = fopen("/proc/net/dev", "r"); char line[512], interface[64];
    uint64_t rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo, rx_frame, rx_compressed, rx_multicast;
    uint64_t tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo, tx_colls, tx_carrier, tx_compressed;
    if (file == NULL) return -1;
    memset(stats, 0, sizeof(*stats));
    while (fgets(line, sizeof(line), file) != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL) continue;
        *colon = ' ';
        if (sscanf(line, " %63s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                   " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                   interface, &rx_bytes, &rx_packets, &rx_errs, &rx_drop, &rx_fifo, &rx_frame, &rx_compressed, &rx_multicast,
                   &tx_bytes, &tx_packets, &tx_errs, &tx_drop, &tx_fifo, &tx_colls, &tx_carrier, &tx_compressed) != 17) continue;
        if (strcmp(interface, "lo") == 0) continue;
        stats->rx_bytes += rx_bytes; stats->rx_packets += rx_packets; stats->rx_errors += rx_errs; stats->rx_drops += rx_drop;
        stats->tx_bytes += tx_bytes; stats->tx_packets += tx_packets; stats->tx_errors += tx_errs; stats->tx_drops += tx_drop;
    }
    fclose(file); return 0;
}

static int collect_sample(sample_t *sample, uint64_t sequence) {
    memset(sample, 0, sizeof(*sample)); sample->sequence = sequence; sample->timestamp_ms = now_ms();
    if (read_cpu_stats(&sample->cpu) != 0 || read_memory_stats(&sample->memory) != 0) return -1;
    (void)read_psi("/proc/pressure/memory", &sample->memory_psi);
    (void)read_psi("/proc/pressure/io", &sample->io_psi);
    (void)read_disk_stats(&sample->disk); (void)read_network_stats(&sample->network);
    return 0;
}

static int serialize_sample(const sample_t *s, char *out, size_t size) {
    int written = snprintf(out, size,
        "{\"v\":%d,\"type\":\"telemetry\",\"seq\":%" PRIu64 ",\"ts\":%" PRId64
        ",\"cpu\":{\"user\":%" PRIu64 ",\"nice\":%" PRIu64 ",\"system\":%" PRIu64 ",\"idle\":%" PRIu64 ",\"iowait\":%" PRIu64 ",\"irq\":%" PRIu64 ",\"softirq\":%" PRIu64 ",\"steal\":%" PRIu64
        "},\"memory_kb\":{\"total\":%" PRIu64 ",\"available\":%" PRIu64 ",\"swap_total\":%" PRIu64 ",\"swap_free\":%" PRIu64
        "},\"psi\":{\"memory\":{\"some_avg10\":%.2f,\"full_avg10\":%.2f,\"some_total_us\":%" PRIu64 ",\"full_total_us\":%" PRIu64
        "},\"io\":{\"some_avg10\":%.2f,\"full_avg10\":%.2f,\"some_total_us\":%" PRIu64 ",\"full_total_us\":%" PRIu64
        "}},\"disk\":{\"read_ops\":%" PRIu64 ",\"read_sectors\":%" PRIu64 ",\"write_ops\":%" PRIu64 ",\"write_sectors\":%" PRIu64 ",\"io_ms\":%" PRIu64
        "},\"network\":{\"rx_bytes\":%" PRIu64 ",\"rx_packets\":%" PRIu64 ",\"rx_errors\":%" PRIu64 ",\"rx_drops\":%" PRIu64 ",\"tx_bytes\":%" PRIu64 ",\"tx_packets\":%" PRIu64 ",\"tx_errors\":%" PRIu64 ",\"tx_drops\":%" PRIu64 "}}\n",
        PROTOCOL_VERSION, s->sequence, s->timestamp_ms, s->cpu.user, s->cpu.nice, s->cpu.system, s->cpu.idle, s->cpu.iowait, s->cpu.irq, s->cpu.softirq, s->cpu.steal,
        s->memory.mem_total_kb, s->memory.mem_available_kb, s->memory.swap_total_kb, s->memory.swap_free_kb,
        s->memory_psi.some_avg10, s->memory_psi.full_avg10, s->memory_psi.some_total_us, s->memory_psi.full_total_us,
        s->io_psi.some_avg10, s->io_psi.full_avg10, s->io_psi.some_total_us, s->io_psi.full_total_us,
        s->disk.read_ops, s->disk.read_sectors, s->disk.write_ops, s->disk.write_sectors, s->disk.io_ms,
        s->network.rx_bytes, s->network.rx_packets, s->network.rx_errors, s->network.rx_drops, s->network.tx_bytes, s->network.tx_packets, s->network.tx_errors, s->network.tx_drops);
    return written >= 0 && (size_t)written < size ? written : -1;
}

static void buffer_push(sample_buffer_t *buffer, const char *line) {
    size_t index = (buffer->head + buffer->count) % SAMPLE_BUFFER_SIZE;
    if (buffer->count == SAMPLE_BUFFER_SIZE) { buffer->head = (buffer->head + 1) % SAMPLE_BUFFER_SIZE; index = (buffer->head + buffer->count - 1) % SAMPLE_BUFFER_SIZE; }
    else buffer->count++;
    snprintf(buffer->lines[index], SAMPLE_LINE_SIZE, "%s", line);
}

static void persist_line(const char *path, const char *line) {
    FILE *file = fopen(path, "a"); if (file == NULL) return;
    (void)fputs(line, file); (void)fflush(file); (void)fclose(file);
}

static void sleep_ms(unsigned int milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    while (running && nanosleep(&request, &request) != 0 && errno == EINTR) {}
}

int main(int argc, char **argv) {
    unsigned int interval_ms = DEFAULT_INTERVAL_MS; const char *spool_path = DEFAULT_SPOOL_PATH;
    int once = 0; uint64_t sequence = 0; sample_buffer_t buffer = {0};
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--once") == 0) once = 1;
        else if (strcmp(argv[index], "--interval-ms") == 0 && index + 1 < argc) interval_ms = (unsigned int)strtoul(argv[++index], NULL, 10);
        else if (strcmp(argv[index], "--spool") == 0 && index + 1 < argc) spool_path = argv[++index];
        else { fprintf(stderr, "uso: %s [--once] [--interval-ms N] [--spool PATH]\n", argv[0]); return EXIT_FAILURE; }
    }
    signal(SIGINT, stop_agent); signal(SIGTERM, stop_agent);
    while (running) {
        sample_t sample; char line[SAMPLE_LINE_SIZE];
        if (collect_sample(&sample, sequence++) != 0 || serialize_sample(&sample, line, sizeof(line)) < 0) fprintf(stderr, "sentrynoc: falha ao coletar métricas\n");
        else { buffer_push(&buffer, line); fputs(line, stdout); fflush(stdout); persist_line(spool_path, line); }
        if (once) break;
        sleep_ms(interval_ms);
    }
    return EXIT_SUCCESS;
}
