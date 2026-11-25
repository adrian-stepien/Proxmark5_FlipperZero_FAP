#include <furi.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>
#include "lf_samples.h"
#include "proxmark5_frame.h"
#include "simple_readers.h"

#define EM_ID_BYTES 5U
// Four RF/64 frames plus one bit for the final edge.
#define EM_SAMPLE_COUNT ((4U * 64U + 1U) * 64U)
#define EM_CARRIER_DIVISOR 95
#define LF_CONFIG_TIMEOUT_MS 1000
#define LF_CAPTURE_TIMEOUT_MS 3000

static int percentile(const uint16_t histogram[256], size_t rank) {
    size_t sum = 0;
    for(int v = 0; v < 256; v++) {
        sum += histogram[v];
        if(sum > rank) return v;
    }
    return 255;
}

static int decode_pulses(
    ProtocolDict* dict,
    const uint8_t* samples,
    size_t count,
    unsigned lower,
    unsigned upper,
    bool invert,
    uint8_t id[EM_ID_BYTES],
    bool* found) {
    protocol_dict_decoders_start(dict);
    bool level = samples[0] >= upper;
    size_t last_edge = 0;
    uint8_t candidate[EM_ID_BYTES];
    bool have_candidate = false;

    for(size_t i = 1; i < count; i++) {
        if(level ? samples[i] > lower : samples[i] < upper) continue;
        const ProtocolId protocol =
            protocol_dict_decoders_feed(dict, level ^ invert, (i - last_edge) * 8U);
        level = !level;
        last_edge = i;
        if(protocol == PROTOCOL_NO) continue;

        uint8_t current[EM_ID_BYTES];
        protocol_dict_get_data(dict, protocol, current, sizeof(current));
        if(!have_candidate || memcmp(current, candidate, sizeof(current)) != 0) {
            memcpy(candidate, current, sizeof(current));
            have_candidate = true;
            continue;
        }
        if(*found && memcmp(id, current, sizeof(current)) != 0) return PM3_EWRONGANSWER;
        memcpy(id, current, sizeof(current));
        *found = true;
    }
    return PM3_SUCCESS;
}

static int decode_em(const uint8_t* samples, size_t count, uint8_t id[EM_ID_BYTES]) {
    uint16_t histogram[256] = {0};
    for(size_t i = 0; i < count; i++) histogram[samples[i]]++;
    const int bottom = percentile(histogram, count / 10);
    const int span = percentile(histogram, count * 9 / 10) - bottom;
    if(span < 8) return PM3_ENODATA;

    const ProtocolBase* modes[] = {
        lfrfid_protocols[LFRFIDProtocolEM4100],
        lfrfid_protocols[LFRFIDProtocolEM410032],
        lfrfid_protocols[LFRFIDProtocolEM410016],
    };
    bool found = false;
    for(int step = 1; step <= 5; step++) {
        const int center = bottom + span * step / 6;
        for(int divider = 3; divider <= 4; divider++) {
            const int lower = center - span / divider;
            const int upper = center + span / divider;
            if(lower < 0 || upper > 255) continue;
            for(unsigned invert = 0; invert < 2; invert++) {
                // Reset the frame and epilogue between threshold and polarity passes.
                ProtocolDict* dict = protocol_dict_alloc(modes, COUNT_OF(modes));
                if(!dict) return PM3_EMALLOC;
                const int status = decode_pulses(dict, samples, count, lower, upper, invert, id, &found);
                protocol_dict_free(dict);
                if(status != PM3_SUCCESS) return status;
            }
        }
    }
    return found ? PM3_SUCCESS : PM3_ENODATA;
}

static int em4100_get_config(sample_config* config) {
    PacketResponseNG reply;
    if(!SendCommandNG(CMD_LF_SAMPLING_GET_CONFIG, NULL, 0)) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_LF_SAMPLING_GET_CONFIG, &reply, LF_CONFIG_TIMEOUT_MS)) {
        return PM3_ETIMEOUT;
    }
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != sizeof(*config)) return PM3_ESOFT;
    memcpy(config, reply.data.asBytes, sizeof(*config));
    return PM3_SUCCESS;
}

static bool same_sampling_config(const sample_config* left, const sample_config* right) {
    return memcmp(left, right, offsetof(sample_config, verbose)) == 0;
}

static int em4100_set_config(const sample_config* config) {
    // The verbose flag requests debug replies and is not stored by SET_CONFIG.
    sample_config request = *config;
    request.verbose = false;
    if(!SendCommandNG(CMD_LF_SAMPLING_SET_CONFIG, (uint8_t*)&request, sizeof(request))) {
        return PM3_EIO;
    }
    // SET_CONFIG has no reply. Wait for a fresh echo before verifying it.
    if(!proxmark5_frame_synchronize()) return PM3_ETIMEOUT;
    sample_config actual;
    int status = em4100_get_config(&actual);
    if(status != PM3_SUCCESS) return status;
    return same_sampling_config(&actual, config) ? PM3_SUCCESS : PM3_ESOFT;
}

static int em4100_capture_samples(void) {
    // 30-bit sample count followed by realtime, verbose and Cotag flags.
    struct {
        uint32_t count;
        uint8_t cotag;
    } PACKED request = {.count = EM_SAMPLE_COUNT};
    PacketResponseNG reply;
    if(!SendCommandNG(CMD_LF_ACQ_RAW_ADC, (uint8_t*)&request, sizeof(request))) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_LF_ACQ_RAW_ADC, &reply, LF_CAPTURE_TIMEOUT_MS)) {
        return PM3_ETIMEOUT;
    }
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != sizeof(uint32_t)) return PM3_ESOFT;
    return reply.data.asDwords[0] == 8U * EM_SAMPLE_COUNT ? PM3_SUCCESS : PM3_EPARTIAL;
}

static int em4100_capture(void) {
    sample_config previous;
    int status = em4100_get_config(&previous);
    if(status != PM3_SUCCESS) return status;

    sample_config config = previous;
    config.decimation = 1;
    config.bits_per_sample = 8;
    config.divisor = EM_CARRIER_DIVISOR;
    config.trigger_threshold = 0;
    config.samples_to_skip = 0;
    const bool changed = !same_sampling_config(&config, &previous);
    if(changed) status = em4100_set_config(&config);
    if(status == PM3_SUCCESS) status = em4100_capture_samples();
    if(changed) {
        if(em4100_set_config(&previous) != PM3_SUCCESS &&
           em4100_set_config(&previous) != PM3_SUCCESS) {
            return SimpleReaderSamplingRestoreFailed;
        }
    }
    return status;
}

static int read_em4100_id(uint8_t id[EM_ID_BYTES]) {
    uint8_t* samples = malloc(EM_SAMPLE_COUNT);
    if(!samples) return PM3_EMALLOC;
    int status = em4100_capture();
    if(status == PM3_SUCCESS) status = lf_samples_download(samples, EM_SAMPLE_COUNT);
    if(status == PM3_SUCCESS) status = decode_em(samples, EM_SAMPLE_COUNT, id);
    free(samples);
    return status;
}

static int em4100_read(SimpleReaderResult* result) {
    uint8_t id[EM_ID_BYTES];
    int status = read_em4100_id(id);
    if(status == PM3_SUCCESS) {
        snprintf(
            result->lines[0], sizeof(result->lines[0]), "ID: %02X%02X%02X%02X%02X",
            id[0], id[1], id[2], id[3], id[4]);
        result->line_count = 1;
    }
    return status;
}

const SimpleReader simple_reader_em4100 = {
    .title = "EM4100 Reader",
    .prompt = "Hold tag to PM5 LF coil",
    .no_data = "No EM4100 ID read",
    .no_data_hint = "Move tag closer",
    .read = em4100_read,
};
