#include "protection.h"

#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"

Protection::~Protection()
{
    const esp_err_t err = deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal melepaskan resource saat destructor: %s",
                 esp_err_to_name(err));
    }
}

esp_err_t Protection::init(const Config &config)
{
    if (initialized_) {
        ESP_LOGE(TAG, "Protection sudah diinisialisasi");
        return ESP_ERR_INVALID_STATE;
    }

    const bool any_sensor_protection =
        config.over_voltage_enabled ||
        config.over_current_enabled ||
        config.over_power_enabled ||
        config.over_temperature_enabled;
    if ((!any_sensor_protection && config.fault_gpio == GPIO_NUM_NC) ||
        (config.over_voltage_enabled &&
         !valid_limit(config.over_voltage_limit_v,
                      config.over_voltage_hysteresis_v)) ||
        (config.over_current_enabled &&
         !valid_limit(config.over_current_limit_a,
                      config.over_current_hysteresis_a)) ||
        (config.over_power_enabled &&
         !valid_limit(config.over_power_limit_w,
                      config.over_power_hysteresis_w)) ||
        (config.over_temperature_enabled &&
         !valid_limit(config.over_temperature_limit_c,
                      config.over_temperature_hysteresis_c)) ||
        (config.over_voltage_enabled &&
         config.over_voltage_debounce_samples == 0) ||
        (config.over_current_enabled &&
         config.over_current_debounce_samples == 0) ||
        (config.over_power_enabled &&
         config.over_power_debounce_samples == 0) ||
        (config.over_temperature_enabled &&
         config.over_temperature_debounce_samples == 0) ||
        (config.fault_gpio != GPIO_NUM_NC &&
         (!valid_gpio(config.fault_gpio) ||
          config.fault_gpio_debounce_samples == 0 ||
          (config.fault_gpio_pull_up && config.fault_gpio_pull_down)))) {
        ESP_LOGE(TAG, "Konfigurasi protection tidak valid");
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;
    if (config_.fault_gpio != GPIO_NUM_NC) {
        gpio_config_t gpio_configure = {};
        gpio_configure.pin_bit_mask =
            1ULL << static_cast<unsigned>(config_.fault_gpio);
        gpio_configure.mode = GPIO_MODE_INPUT;
        gpio_configure.pull_up_en =
            config_.fault_gpio_pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        gpio_configure.pull_down_en =
            config_.fault_gpio_pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        gpio_configure.intr_type = GPIO_INTR_DISABLE;

        const esp_err_t err = gpio_config(&gpio_configure);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gagal mengonfigurasi GPIO fault: %s",
                     esp_err_to_name(err));
            return err;
        }
        fault_gpio_configured_ = true;
    }

    latched_fault_ = protection_fault_t::None;
    output_active_ = false;
    output_activated_at_us_ = 0;
    reset_counters();
    initialized_ = true;
    ESP_LOGI(TAG, "Proteksi software siap");
    return ESP_OK;
}

esp_err_t Protection::deinit()
{
    esp_err_t err = ESP_OK;
    if (fault_gpio_configured_) {
        err = gpio_reset_pin(config_.fault_gpio);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gagal mereset GPIO fault: %s", esp_err_to_name(err));
        } else {
            fault_gpio_configured_ = false;
        }
    }

    if (err == ESP_OK) {
        initialized_ = false;
        output_active_ = false;
        output_activated_at_us_ = 0;
        reset_counters();
    }
    return err;
}

esp_err_t Protection::set_output_active(bool active)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (output_active_ == active) {
        return ESP_OK;
    }

    output_active_ = active;
    reset_counters();
    if (active) {
        output_activated_at_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Pemantauan output dimulai");
    } else {
        output_activated_at_us_ = 0;
    }
    return ESP_OK;
}

esp_err_t Protection::update(const Sample &sample,
                             protection_fault_t *out_fault)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_fault == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_fault = latched_fault_;

    if (!output_active_ || latched_fault_ != protection_fault_t::None) {
        return ESP_OK;
    }

    if (sample.sensor_error) {
        const esp_err_t err = latch_fault(protection_fault_t::SensorError);
        *out_fault = latched_fault_;
        return err;
    }
    if (!sample_valid(sample)) {
        const esp_err_t err = latch_fault(protection_fault_t::InvalidMeasurement);
        *out_fault = latched_fault_;
        return err;
    }

    bool gpio_fault_active = false;
    const esp_err_t gpio_err = read_fault_gpio(&gpio_fault_active);
    if (gpio_err != ESP_OK) {
        return gpio_err;
    }
    if (config_.fault_gpio != GPIO_NUM_NC &&
        threshold_reached(gpio_fault_active,
                          config_.fault_gpio_debounce_samples,
                          &fault_gpio_count_)) {
        const esp_err_t err = latch_fault(protection_fault_t::ShortCircuit);
        *out_fault = latched_fault_;
        return err;
    }

    if (config_.over_temperature_enabled &&
        threshold_reached(sample.temperature_c >=
                              config_.over_temperature_limit_c,
                          config_.over_temperature_debounce_samples,
                          &over_temperature_count_)) {
        const esp_err_t err = latch_fault(protection_fault_t::OverTemperature);
        *out_fault = latched_fault_;
        return err;
    }

    if (!startup_grace_active()) {
        if (config_.over_voltage_enabled &&
            threshold_reached(sample.voltage_v >= config_.over_voltage_limit_v,
                              config_.over_voltage_debounce_samples,
                              &over_voltage_count_)) {
            const esp_err_t err = latch_fault(protection_fault_t::OverVoltage);
            *out_fault = latched_fault_;
            return err;
        }
        if (config_.over_current_enabled &&
            threshold_reached(sample.current_a >= config_.over_current_limit_a,
                              config_.over_current_debounce_samples,
                              &over_current_count_)) {
            const esp_err_t err = latch_fault(protection_fault_t::OverCurrent);
            *out_fault = latched_fault_;
            return err;
        }
        if (config_.over_power_enabled &&
            threshold_reached(sample.power_w >= config_.over_power_limit_w,
                              config_.over_power_debounce_samples,
                              &over_power_count_)) {
            const esp_err_t err = latch_fault(protection_fault_t::OverPower);
            *out_fault = latched_fault_;
            return err;
        }
    } else {
        over_voltage_count_ = 0;
        over_current_count_ = 0;
        over_power_count_ = 0;
    }

    return ESP_OK;
}

esp_err_t Protection::clear_fault(const Sample &sample)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (latched_fault_ == protection_fault_t::None) {
        return ESP_ERR_INVALID_STATE;
    }
    if (output_active_) {
        ESP_LOGE(TAG, "Matikan output sebelum menghapus fault");
        return ESP_ERR_INVALID_STATE;
    }
    if (sample.sensor_error || !sample_valid(sample) ||
        !sample_recovered(sample)) {
        ESP_LOGE(TAG, "Kondisi sensor belum valid atau belum pulih");
        return ESP_ERR_INVALID_STATE;
    }

    bool gpio_fault_active = false;
    const esp_err_t gpio_err = read_fault_gpio(&gpio_fault_active);
    if (gpio_err != ESP_OK) {
        return gpio_err;
    }
    if (gpio_fault_active) {
        ESP_LOGE(TAG, "Input GPIO fault masih aktif");
        return ESP_ERR_INVALID_STATE;
    }

    latched_fault_ = protection_fault_t::None;
    reset_counters();
    ESP_LOGI(TAG, "Fault dihapus secara manual");
    return ESP_OK;
}

protection_fault_t Protection::get_fault() const
{
    return latched_fault_;
}

bool Protection::is_initialized() const
{
    return initialized_;
}

bool Protection::is_output_active() const
{
    return output_active_;
}

bool Protection::valid_gpio(gpio_num_t gpio)
{
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

bool Protection::valid_limit(float limit, float hysteresis)
{
    return std::isfinite(limit) &&
           std::isfinite(hysteresis) &&
           limit > 0.0f &&
           hysteresis >= 0.0f &&
           hysteresis < limit;
}

bool Protection::sample_value_valid(float value, bool marked_valid,
                                    bool allow_negative)
{
    return marked_valid && std::isfinite(value) &&
           (allow_negative || value >= 0.0f);
}

bool Protection::sample_valid(const Sample &sample) const
{
    return (!config_.over_voltage_enabled ||
            sample_value_valid(sample.voltage_v, sample.voltage_valid, false)) &&
           (!config_.over_current_enabled ||
            sample_value_valid(sample.current_a, sample.current_valid, false)) &&
           (!config_.over_power_enabled ||
            sample_value_valid(sample.power_w, sample.power_valid, false)) &&
           (!config_.over_temperature_enabled ||
            sample_value_valid(sample.temperature_c,
                               sample.temperature_valid, true));
}

bool Protection::sample_recovered(const Sample &sample) const
{
    return (!config_.over_voltage_enabled ||
            sample.voltage_v <
                config_.over_voltage_limit_v - config_.over_voltage_hysteresis_v) &&
           (!config_.over_current_enabled ||
            sample.current_a <
                config_.over_current_limit_a - config_.over_current_hysteresis_a) &&
           (!config_.over_power_enabled ||
            sample.power_w <
                config_.over_power_limit_w - config_.over_power_hysteresis_w) &&
           (!config_.over_temperature_enabled ||
            sample.temperature_c <
                config_.over_temperature_limit_c -
                    config_.over_temperature_hysteresis_c);
}

bool Protection::startup_grace_active() const
{
    if (config_.startup_grace_period_ms == 0 || !output_active_) {
        return false;
    }
    const int64_t elapsed_us = esp_timer_get_time() - output_activated_at_us_;
    return elapsed_us >= 0 &&
           elapsed_us < static_cast<int64_t>(config_.startup_grace_period_ms) *
                            1000;
}

void Protection::reset_counters()
{
    over_voltage_count_ = 0;
    over_current_count_ = 0;
    over_power_count_ = 0;
    over_temperature_count_ = 0;
    fault_gpio_count_ = 0;
}

bool Protection::threshold_reached(bool condition,
                                   uint16_t required_samples,
                                   uint16_t *counter)
{
    if (counter == nullptr) {
        return false;
    }
    if (!condition) {
        *counter = 0;
        return false;
    }
    if (*counter < required_samples) {
        ++(*counter);
    }
    return *counter >= required_samples;
}

esp_err_t Protection::read_fault_gpio(bool *active) const
{
    if (active == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *active = false;
    if (config_.fault_gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }

    const int level = gpio_get_level(config_.fault_gpio);
    if (level != 0 && level != 1) {
        ESP_LOGE(TAG, "Gagal membaca GPIO fault");
        return ESP_ERR_INVALID_RESPONSE;
    }
    *active = (level == 1) == config_.fault_gpio_active_high;
    return ESP_OK;
}

esp_err_t Protection::latch_fault(protection_fault_t fault)
{
    if (latched_fault_ != protection_fault_t::None) {
        return ESP_OK;
    }
    latched_fault_ = fault;
    ESP_LOGE(TAG, "Fault dilatch: %u", static_cast<unsigned>(fault));
    if (config_.on_fault != nullptr) {
        return config_.on_fault(fault, config_.callback_context);
    }
    return ESP_OK;
}
