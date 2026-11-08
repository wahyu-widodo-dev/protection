# Protection

Komponen C++ ESP-IDF untuk memantau batas keselamatan software pada power
supply unit (PSU): over-voltage (OVP), over-current (OCP), over-power (OPP),
over-temperature (OTP), fault GPIO eksternal, dan data sensor tidak valid.

Ambang proteksi dapat ditetapkan pada `init()` dan diubah saat aplikasi berjalan
melalui method `set_over_*_limit()`. Komponen ini tidak menetapkan hubungan
otomatis antara setpoint aplikasi dan batas proteksi: saat setpoint berubah,
aplikasi menentukan dan mengirim batas perlindungan yang sesuai.

> **Peringatan keselamatan:** proteksi software bukan pengganti fuse, current
> limiter, thermal cutoff, OVP/OCP hardware, atau interlock. Firmware hanya
> dapat bereaksi jika MCU, sensor, dan jalur software masih berfungsi. Untuk
> mencegah kerusakan atau bahaya, sediakan proteksi hardware independen.

## Fitur

- OVP, OCP, OPP opsional, OTP, sensor fault, dan input fault digital opsional.
- Ambang, hysteresis, dan enable/disable tiap proteksi dapat diubah saat runtime.
- Debounce berdasarkan jumlah sampel berurutan.
- Startup grace period opsional untuk OVP/OCP/OPP.
- Fault latch dan penghapusan manual setelah output nonaktif dan kondisi pulih.
- Callback opsional ketika fault pertama kali terjadi untuk meminta aplikasi
  mematikan output.
- Tidak terikat pada sensor, `DataShared`, komponen `output`, atau format
  konfigurasi proyek tertentu.

## Persyaratan dan instalasi

- ESP-IDF **5.3 atau lebih baru**.
- Aplikasi menyediakan dan membaca sensor, lalu memanggil `update()` berkala.
- Setelah komponen dipublikasikan di Registry, tambahkan ke manifest proyek:

```yaml
dependencies:
  wahyu-widodo-dev/protection:
    version: "^1.0.0"
```

Jalankan `idf.py reconfigure` untuk mengambil dependency. Untuk pengembangan
lokal, letakkan folder ini di `components/` atau daftarkan lokasinya dengan
`EXTRA_COMPONENT_DIRS`.

## Inisialisasi

Contoh ini menggunakan konfigurasi awal. Sesuaikan seluruh batas, satuan,
GPIO, dan polaritas dengan rangkaian serta kebijakan aplikasi.

```cpp
#include "protection.h"

static Protection protection;

void start_protection()
{
    Protection::Config config;
    config.over_voltage_limit_v = 15.0f;
    config.over_voltage_hysteresis_v = 0.5f;
    config.over_current_limit_a = 3.0f;
    config.over_current_hysteresis_a = 0.1f;
    config.over_power_enabled = true;
    config.over_power_limit_w = 40.0f;
    config.over_power_hysteresis_w = 1.0f;
    config.over_temperature_limit_c = 80.0f;
    config.over_temperature_hysteresis_c = 5.0f;

    // Opsional: sinyal fault digital dari rangkaian eksternal.
    config.fault_gpio = GPIO_NUM_4;
    config.fault_gpio_active_high = true;
    config.fault_gpio_pull_down = true;
    config.fault_gpio_pull_up = false;

    config.over_voltage_debounce_samples = 3;
    config.over_current_debounce_samples = 3;
    config.over_power_debounce_samples = 3;
    config.over_temperature_debounce_samples = 3;
    config.startup_grace_period_ms = 250;

    ESP_ERROR_CHECK(protection.init(config));
}
```

Jika input fault eksternal tidak digunakan, biarkan `fault_gpio` pada
`GPIO_NUM_NC`. OPP dapat dinonaktifkan bila sistem tidak memiliki pembacaan
daya yang valid.

## Mengubah batas saat runtime

Setiap setter menerima batas dan hysteresis sekaligus supaya keduanya divalidasi
dan diperbarui bersama. Setter dapat digunakan saat output
sedang berjalan. Perubahan berlaku pada pemanggilan `update()` berikutnya dan
mereset penghitung debounce untuk proteksi tersebut. Setter tidak menghapus
fault yang sudah dilatch.

```cpp
void apply_voltage_setpoint(float voltage_setpoint_v)
{
    // Kebijakan contoh aplikasi: OVP disetel 20% di atas setpoint.
    // Rasio ini keputusan aplikasi, bukan aturan bawaan komponen Protection.
    const float ovp_limit_v = voltage_setpoint_v * 1.20f;
    const float ovp_hysteresis_v = 0.5f;

    ESP_ERROR_CHECK(protection.set_over_voltage_limit(
        ovp_limit_v, ovp_hysteresis_v));
}

void apply_current_setpoint(float current_setpoint_a)
{
    const float ocp_limit_a = current_setpoint_a * 1.10f;
    const float ocp_hysteresis_a = 0.1f;

    ESP_ERROR_CHECK(protection.set_over_current_limit(
        ocp_limit_a, ocp_hysteresis_a));
}
```

Setter yang tersedia:

| Method | Parameter |
|---|---|
| `set_over_voltage_limit(limit_v, hysteresis_v)` | Ambang OVP volt dan hysteresis volt. |
| `set_over_current_limit(limit_a, hysteresis_a)` | Ambang OCP ampere dan hysteresis ampere. |
| `set_over_power_limit(limit_w, hysteresis_w)` | Ambang OPP watt dan hysteresis watt. |
| `set_over_temperature_limit(limit_c, hysteresis_c)` | Ambang OTP Celsius dan hysteresis Celsius. |
| `set_over_voltage_enabled(enabled)` | Aktif/nonaktif OVP. |
| `set_over_current_enabled(enabled)` | Aktif/nonaktif OCP. |
| `set_over_power_enabled(enabled)` | Aktif/nonaktif OPP. |
| `set_over_temperature_enabled(enabled)` | Aktif/nonaktif OTP. |

Batas harus finite dan lebih besar dari nol. Hysteresis harus finite, tidak
negatif, dan lebih kecil dari batas. Nilai invalid menghasilkan
`ESP_ERR_INVALID_ARG`; memanggil setter sebelum `init()` menghasilkan
`ESP_ERR_INVALID_STATE`. Proteksi yang dinonaktifkan tidak diperiksa dan tidak
mewajibkan channel sensor terkait valid.

Sebaiknya panggil setter dan `update()` secara serial dari task kontrol yang
sama, atau lindungi akses objek dengan sinkronisasi aplikasi. Jangan mengubah
konfigurasi `Protection` secara bersamaan dari beberapa task.

## Siklus pemeriksaan

```cpp
void protection_control_step(float voltage_v,
                             float current_a,
                             float power_w,
                             float temperature_c)
{
    Protection::Sample sample;
    sample.voltage_v = voltage_v;
    sample.current_a = current_a;
    sample.power_w = power_w;
    sample.temperature_c = temperature_c;
    sample.voltage_valid = true;
    sample.current_valid = true;
    sample.power_valid = true;
    sample.temperature_valid = true;

    protection_fault_t fault = protection_fault_t::None;
    const esp_err_t err = protection.update(sample, &fault);
    if (err != ESP_OK) {
        // Tangani kesalahan runtime dengan kebijakan aman milik aplikasi.
        return;
    }

    if (fault != protection_fault_t::None) {
        // Output harus dinonaktifkan atau sudah diminta nonaktif oleh callback.
        // Tampilkan/catat fault dan jangan aktifkan kembali secara otomatis.
    }
}
```

Panggil `set_output_active(true)` setelah output berhasil diaktifkan dan
`set_output_active(false)` setelah berhasil dinonaktifkan. Ini menentukan kapan
proteksi berjalan dan kapan startup grace dimulai. Fault GPIO dan OTP tetap
diperiksa selama startup grace; grace hanya menunda OVP, OCP, dan OPP.

## Menangani sensor yang tidak tersedia

Untuk proteksi yang aktif, setiap sampel harus menandai channel terkait valid:

| Proteksi | Field yang wajib valid |
|---|---|
| OVP | `voltage_v`, `voltage_valid` |
| OCP | `current_a`, `current_valid` |
| OPP | `power_w`, `power_valid` |
| OTP | `temperature_c`, `temperature_valid` |

Jika pembacaan sensor gagal, set `sample.sensor_error = true` dan panggil
`update()`. Komponen akan melatch `SensorError`. Nilai tegangan, arus, dan daya
negatif atau tidak finite dianggap tidak valid. Suhu negatif diperbolehkan
karena dapat menjadi pembacaan Celsius yang sah.

## Callback fault

Callback dipanggil satu kali saat fault pertama kali dilatch. Callback dapat
memanggil API komponen output untuk menonaktifkan PSU:

```cpp
#include "output.h"

static Output *psu_output = nullptr;

static esp_err_t disable_output_on_fault(protection_fault_t, void *context)
{
    auto *output = static_cast<Output *>(context);
    return output->output_disable();
}

void configure_fault_callback(Protection::Config &config, Output &output)
{
    psu_output = &output;
    config.on_fault = disable_output_on_fault;
    config.callback_context = psu_output;
}
```

Inisialisasi output sebelum `Protection`, agar callback tidak mengakses objek
yang belum siap. Jika callback gagal, fault tetap dilatch dan `update()`
mengembalikan error callback; aplikasi tetap harus menjalankan jalur shutdown
aman yang sesuai.

## Pemulihan dan fault latch

Fault tidak terhapus otomatis setelah nilai kembali normal. Urutan yang
disarankan:

1. Pastikan output PSU nonaktif.
2. Selesaikan penyebab fault dan pastikan input GPIO eksternal tidak aktif.
3. Setelah output mati, ambil pembacaan sensor baru yang valid dan kirim
   melalui `update(sample, &fault)`.
4. Pastikan setiap channel proteksi aktif berada di bawah
   `batas - hysteresis`.
5. Panggil `clear_fault()` dan periksa hasilnya.
6. Aktifkan kembali output hanya melalui kebijakan aplikasi.

`clear_fault()` menggunakan sampel terakhir yang dikirim ke `update()`. Ia
mensyaratkan ada sampel baru setelah output dinyatakan nonaktif, lalu menolak
permintaan jika input fault masih aktif, data sensor tidak valid, atau salah
satu nilai belum pulih. Mengubah batas atau menonaktifkan proteksi tidak
menghapus fault yang sudah dilatch.

```cpp
ESP_ERROR_CHECK(output.output_disable());
ESP_ERROR_CHECK(protection.set_output_active(false));

Protection::Sample recovered_sample = read_current_sensor_sample();
protection_fault_t fault = protection_fault_t::None;
ESP_ERROR_CHECK(protection.update(recovered_sample, &fault));
ESP_ERROR_CHECK(protection.clear_fault());
```

## Konfigurasi

| Field | Default | Keterangan |
|---|---:|---|
| `over_voltage_enabled` | `true` | Aktifkan OVP. |
| `over_voltage_limit_v` | `15.0` | Ambang OVP volt; dapat diubah dengan setter. |
| `over_voltage_hysteresis_v` | `0.5` | Hysteresis OVP; dapat diubah dengan setter. |
| `over_current_enabled` | `true` | Aktifkan OCP. |
| `over_current_limit_a` | `3.0` | Ambang OCP ampere; dapat diubah dengan setter. |
| `over_current_hysteresis_a` | `0.1` | Hysteresis OCP; dapat diubah dengan setter. |
| `over_power_enabled` | `false` | Aktifkan OPP bila pengukuran daya tersedia. |
| `over_power_limit_w` | `40.0` | Ambang OPP watt; dapat diubah dengan setter. |
| `over_power_hysteresis_w` | `1.0` | Hysteresis OPP; dapat diubah dengan setter. |
| `over_temperature_enabled` | `true` | Aktifkan OTP. |
| `over_temperature_limit_c` | `80.0` | Ambang OTP Celsius; dapat diubah dengan setter. |
| `over_temperature_hysteresis_c` | `5.0` | Hysteresis OTP; dapat diubah dengan setter. |
| `fault_gpio` | `GPIO_NUM_NC` | GPIO input opsional dari rangkaian eksternal. |
| `fault_gpio_active_high` | `true` | Level aktif input GPIO fault. |
| `fault_gpio_pull_up` | `false` | Pull-up internal opsional. |
| `fault_gpio_pull_down` | `true` | Pull-down internal opsional. |
| `fault_gpio_debounce_samples` | `1` | Sampel aktif berurutan sebelum fault GPIO. |
| `*_debounce_samples` | `3` | Sampel berurutan di atas ambang sebelum sensor fault. |
| `startup_grace_period_ms` | `0` | Penundaan OVP/OCP/OPP setelah output diaktifkan. |
| `on_fault` | `nullptr` | Callback saat fault pertama dilatch. |
| `callback_context` | `nullptr` | Konteks callback. |

Debounce menghitung sampel, bukan waktu. Tiga sampel pada interval 10 ms
berarti sekitar 30 ms; waktu aktual mengikuti periode pemanggilan `update()`.
Gunakan startup grace sesingkat yang dibutuhkan sistem.

## API lainnya

| API | Fungsi |
|---|---|
| `init(config)` | Validasi konfigurasi awal dan siapkan GPIO opsional. |
| `deinit()` | Lepaskan GPIO dan hentikan lifecycle. |
| `set_output_active(active)` | Beri tahu komponen tentang status output dan mulai grace saat aktif. |
| `update(sample, &fault)` | Evaluasi sampel terbaru dan kembalikan status fault. |
| `clear_fault()` | Hapus fault dengan memvalidasi sampel terbaru dari `update()`. |
| `get_fault()` | Baca fault yang dilatch. |
| `is_initialized()` | Baca status inisialisasi. |
| `is_output_active()` | Baca status output yang diberitahukan aplikasi. |

## Jenis fault

`protection_fault_t` menyediakan `None`, `OverVoltage`, `OverCurrent`,
`OverPower`, `OverTemperature`, `ShortCircuit`, `SensorError`, dan
`InvalidMeasurement`.

## Batas keselamatan

- Komponen tidak membaca sensor atau membuat task kontrol.
- Komponen tidak mengendalikan output secara langsung kecuali aplikasi
  menghubungkan `on_fault` ke API shutdown output.
- `set_output_active()` harus mengikuti status output yang sebenarnya.
- Jangan mengandalkan firmware sebagai satu-satunya lapisan proteksi PSU.
- Uji setiap batas, transien, dan kebijakan pemulihan pada perangkat keras
  dengan prosedur yang aman.

## Lisensi

Komponen ini didistribusikan di bawah lisensi MIT. Lihat [LICENSE](./LICENSE).
