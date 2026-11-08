#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

/**
 * @brief Fault yang dapat dideteksi oleh komponen Protection.
 */
enum class protection_fault_t : uint8_t {
    None = 0,
    OverVoltage,
    OverCurrent,
    OverPower,
    OverTemperature,
    ShortCircuit,
    SensorError,
    InvalidMeasurement,
};

/**
 * @brief Pemeriksa proteksi software untuk sistem PSU.
 *
 * Komponen memeriksa sampel yang diberikan aplikasi dan melatch fault sampai
 * aplikasi mematikan output serta memanggil clear_fault() secara eksplisit.
 * Komponen tidak mengendalikan tahap daya secara langsung.
 */
class Protection
{
public:
    struct Sample {
        float voltage_v = 0.0f;
        float current_a = 0.0f;
        float power_w = 0.0f;
        float temperature_c = 0.0f;
        bool voltage_valid = false;
        bool current_valid = false;
        bool power_valid = false;
        bool temperature_valid = false;
        bool sensor_error = false;
    };

    using FaultCallback = esp_err_t (*)(protection_fault_t fault, void *context);

    struct Config {
        bool over_voltage_enabled = true;
        float over_voltage_limit_v = 0.0f;
        float over_voltage_hysteresis_v = 0.2f;

        bool over_current_enabled = true;
        float over_current_limit_a = 0.0f;
        float over_current_hysteresis_a = 0.05f;

        bool over_power_enabled = false;
        float over_power_limit_w = 0.0f;
        float over_power_hysteresis_w = 0.5f;

        bool over_temperature_enabled = true;
        float over_temperature_limit_c = 0.0f;
        float over_temperature_hysteresis_c = 2.0f;

        gpio_num_t fault_gpio = GPIO_NUM_NC;
        bool fault_gpio_active_high = true;
        bool fault_gpio_pull_up = false;
        bool fault_gpio_pull_down = true;
        uint16_t fault_gpio_debounce_samples = 1;

        uint16_t over_voltage_debounce_samples = 3;
        uint16_t over_current_debounce_samples = 3;
        uint16_t over_power_debounce_samples = 3;
        uint16_t over_temperature_debounce_samples = 3;
        uint32_t startup_grace_period_ms = 0;

        FaultCallback on_fault = nullptr;
        void *callback_context = nullptr;
    };

    Protection() = default;
    ~Protection();

    Protection(const Protection &) = delete;
    Protection &operator=(const Protection &) = delete;
    Protection(Protection &&) = delete;
    Protection &operator=(Protection &&) = delete;

    /** Validasi konfigurasi dan siapkan input GPIO fault jika digunakan. */
    esp_err_t init(const Config &config);

    /** Lepaskan GPIO yang dikelola komponen dan hentikan lifecycle komponen. */
    esp_err_t deinit();

    /**
     * @brief Tandai apakah keluaran PSU sedang aktif.
     *
     * Masa startup grace dimulai ulang setiap kali output berubah dari nonaktif
     * menjadi aktif. Fault yang sudah dilatch tidak dihapus oleh fungsi ini.
     */
    esp_err_t set_output_active(bool active);

    /**
     * @brief Periksa sampel sensor dan kembalikan fault yang sedang dilatch.
     *
     * Sensor yang proteksinya aktif wajib menandai nilai valid terkait dengan
     * flag *_valid. Fault callback dipanggil satu kali saat fault pertama
     * dilatch. Jika callback gagal, error callback dikembalikan tetapi fault
     * tetap dilatch.
     */
    esp_err_t update(const Sample &sample, protection_fault_t *out_fault);

    /**
     * @brief Hapus fault secara manual setelah output dimatikan dan kondisi pulih.
     *
     * Pembacaan harus valid, seluruh proteksi terkonfigurasi harus berada di
     * bawah batas pemulihan (limit - hysteresis), dan input fault GPIO harus
     * sudah tidak aktif.
     */
    esp_err_t clear_fault(const Sample &sample);

    protection_fault_t get_fault() const;
    bool is_initialized() const;
    bool is_output_active() const;

private:
    static constexpr const char *TAG = "protection";

    Config config_{};
    protection_fault_t latched_fault_ = protection_fault_t::None;
    int64_t output_activated_at_us_ = 0;
    uint16_t over_voltage_count_ = 0;
    uint16_t over_current_count_ = 0;
    uint16_t over_power_count_ = 0;
    uint16_t over_temperature_count_ = 0;
    uint16_t fault_gpio_count_ = 0;
    bool initialized_ = false;
    bool output_active_ = false;
    bool fault_gpio_configured_ = false;

    static bool valid_gpio(gpio_num_t gpio);
    static bool valid_limit(float limit, float hysteresis);
    static bool sample_value_valid(float value, bool marked_valid,
                                   bool allow_negative);
    bool sample_valid(const Sample &sample) const;
    bool sample_recovered(const Sample &sample) const;
    bool startup_grace_active() const;
    void reset_counters();
    bool threshold_reached(bool condition, uint16_t required_samples,
                           uint16_t *counter);
    esp_err_t read_fault_gpio(bool *active) const;
    esp_err_t latch_fault(protection_fault_t fault);
};
