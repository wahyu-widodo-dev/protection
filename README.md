# Protection

Komponen C++ untuk memantau kondisi berbahaya pada power supply unit (PSU)
melalui pembacaan sensor dan, bila tersedia, sinyal fault digital eksternal.
Komponen mendeteksi over-voltage (OVP), over-current (OCP), over-power (OPP),
over-temperature (OTP), hubung singkat, error sensor, dan data pengukuran yang
tidak valid.

Saat mendeteksi fault, komponen akan melatch penyebabnya dan memanggil callback
opsional agar aplikasi dapat segera menonaktifkan output. Fault hanya dapat
dihapus secara eksplisit setelah output nonaktif dan kondisi pengukuran pulih.

> **Peringatan keselamatan:** komponen ini adalah lapisan proteksi software,
> bukan pengganti fuse, current limiter, thermal cutoff, OVP/OCP hardware,
> rangkaian interlock, atau desain isolasi yang tepat. Proteksi software hanya
> bekerja jika MCU, sensor, firmware, dan jalur output masih berfungsi. Untuk
> batas yang berisiko merusak perangkat atau membahayakan pengguna, gunakan
> proteksi hardware independen yang bereaksi tanpa firmware.

## Fitur

- Proteksi OVP, OCP, OPP opsional, dan OTP dengan batas serta hysteresis
  pemulihan yang dikonfigurasi terpisah.
- Input digital opsional untuk indikator fault eksternal atau short-circuit.
- Debounce berbasis jumlah sampel berurutan; tidak mengasumsikan periode loop
  tertentu.
- Startup grace period opsional untuk OVP/OCP/OPP saat output baru diaktifkan.
  OTP, input fault GPIO, serta validitas sensor tetap diperiksa selama grace.
- Fault latch: fault tidak hilang hanya karena pembacaan kembali normal.
- Callback fault opsional agar aplikasi dapat menonaktifkan output pada saat
  fault terdeteksi.
- Validasi data sensor; channel yang diproteksi wajib ditandai valid.
- Tidak bergantung pada `DataShared`, komponen `utils`, sensor tertentu, atau
  komponen `output`.
- API mengembalikan `esp_err_t` agar aplikasi dapat menangani kesalahan secara
  eksplisit.

## Persyaratan

- ESP-IDF **5.3 atau lebih baru**.
- Sensor tegangan/arus/daya/suhu disediakan dan dibaca oleh aplikasi.
- Aplikasi harus memanggil `update()` secara berkala saat output aktif.
- Gunakan `fault_gpio` hanya bila tersedia sinyal fault digital eksternal yang
  level aktifnya diketahui.

## Instalasi

Setelah komponen tersedia di ESP Component Registry, tambahkan ke
`idf_component.yml` proyek:

```yaml
dependencies:
  wahyu-widodo-dev/protection:
    version: "^1.0.0"
```

Kemudian konfigurasi ulang proyek:

```sh
idf.py reconfigure
```

Untuk memakai salinan lokal, letakkan folder komponen ini di dalam
`components/` proyek atau daftarkan lokasinya melalui `EXTRA_COMPONENT_DIRS`.

## Penggunaan

Contoh berikut menghubungkan fault callback ke komponen `Output`. Callback
menjalankan `output_disable()` tepat saat fault pertama kali dilatch.
Pastikan `output` sudah diinisialisasi dengan bus, pin, dan konfigurasi PSU
sebelum menjalankan `start_protection()`. Sesuaikan pin, batas, sensor, dan
polaritas dengan perangkat keras Anda.

```cpp
#include "output.h"
#include "protection.h"

static Output output;
static Protection protection;

static esp_err_t disable_output_on_fault(protection_fault_t, void *context)
{
    auto *psu_output = static_cast<Output *>(context);
    return psu_output->output_disable();
}

void start_protection()
{
    Protection::Config config;

    config.over_voltage_enabled = true;
    config.over_voltage_limit_v = 15.0f;
    config.over_voltage_hysteresis_v = 0.5f;

    config.over_current_enabled = true;
    config.over_current_limit_a = 3.0f;
    config.over_current_hysteresis_a = 0.1f;

    config.over_power_enabled = true;
    config.over_power_limit_w = 40.0f;
    config.over_power_hysteresis_w = 1.0f;

    config.over_temperature_enabled = true;
    config.over_temperature_limit_c = 80.0f;
    config.over_temperature_hysteresis_c = 5.0f;

    // Contoh input fault aktif-high. Pastikan level ini cocok dengan
    // rangkaian eksternal dan konfigurasi pull resistor pada board.
    config.fault_gpio = GPIO_NUM_4;
    config.fault_gpio_active_high = true;
    config.fault_gpio_pull_down = true;
    config.fault_gpio_pull_up = false;
    config.fault_gpio_debounce_samples = 1;

    config.over_voltage_debounce_samples = 3;
    config.over_current_debounce_samples = 3;
    config.over_power_debounce_samples = 3;
    config.over_temperature_debounce_samples = 3;
    config.startup_grace_period_ms = 250;

    config.on_fault = disable_output_on_fault;
    config.callback_context = &output;

    ESP_ERROR_CHECK(protection.init(config));

    // Aktifkan output melalui API output setelah konfigurasi dan pemeriksaan
    // aplikasi selesai. Tandai aktif hanya setelah aktivasi berhasil.
    ESP_ERROR_CHECK(output.output_enable());
    ESP_ERROR_CHECK(protection.set_output_active(true));
}

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
        // Contoh kebijakan konservatif: jangan lanjutkan operasi normal jika
        // pemeriksaan protection gagal.
        output.output_disable();
        return;
    }

    if (fault != protection_fault_t::None) {
        // Fault sudah dilatch. Callback juga sudah mencoba menonaktifkan output.
        // Aplikasi dapat mencatat kode fault atau memberi tahu pengguna.
    }
}

void stop_protection()
{
    ESP_ERROR_CHECK(output.output_disable());
    ESP_ERROR_CHECK(protection.set_output_active(false));
    ESP_ERROR_CHECK(protection.deinit());
}
```

### Menangani sensor yang tidak tersedia

Aktifkan hanya proteksi yang didukung oleh sensor pada perangkat. Setiap
proteksi yang aktif mewajibkan field terkait valid pada setiap sampel:

| Proteksi | Field yang harus valid |
|---|---|
| OVP | `voltage_v`, `voltage_valid` |
| OCP | `current_a`, `current_valid` |
| OPP | `power_w`, `power_valid` |
| OTP | `temperature_c`, `temperature_valid` |

Jika pembacaan sensor gagal, set `sample.sensor_error = true` dan panggil
`update()`. Komponen akan melatch `SensorError` tanpa menunggu debounce.
Jangan isi nilai perkiraan atau nilai lama lalu tandai valid setelah transaksi
sensor gagal.

Proteksi OPP opsional karena beberapa aplikasi menghitung daya dari tegangan
dan arus. Bila dihitung di aplikasi, validasi kedua pengukuran terlebih dahulu
dan tandai `power_valid = true` hanya bila hasil perhitungan valid.

### Menghapus fault

`clear_fault()` tidak mengaktifkan kembali output. Alur pemulihan yang
direkomendasikan:

1. Pastikan output sudah nonaktif, termasuk bila callback fault gagal.
2. Selesaikan penyebab fault dan pastikan input GPIO eksternal sudah tidak aktif.
3. Ambil sampel sensor baru yang valid.
4. Pastikan semua nilai berada di bawah batas pemulihan:
   `limit - hysteresis` untuk setiap proteksi yang aktif.
5. Panggil `clear_fault(sample)`.
6. Periksa hasilnya, lalu aktifkan output kembali hanya melalui kebijakan
   aplikasi.

Contoh:

```cpp
Protection::Sample recovered_sample;
recovered_sample.voltage_v = 5.0f;
recovered_sample.current_a = 0.0f;
recovered_sample.power_w = 0.0f;
recovered_sample.temperature_c = 30.0f;
recovered_sample.voltage_valid = true;
recovered_sample.current_valid = true;
recovered_sample.power_valid = true;
recovered_sample.temperature_valid = true;

const esp_err_t err = protection.clear_fault(recovered_sample);
if (err == ESP_OK) {
    // Fault sudah dihapus, tetapi output tetap nonaktif sampai aplikasi
    // memutuskan untuk mengaktifkannya kembali.
}
```

Semua field terkait proteksi aktif harus valid bahkan jika fault asalnya hanya
terkait satu channel. Ketentuan ini mencegah fault dihapus saat sensor lain
yang dipantau sedang gagal.

## API

| API | Fungsi |
|---|---|
| `init(config)` | Memvalidasi konfigurasi dan menyiapkan GPIO fault opsional. |
| `deinit()` | Melepaskan pin GPIO yang dikonfigurasi dan menghentikan lifecycle komponen. |
| `set_output_active(active)` | Memberi tahu komponen kapan output mulai/berhenti aktif. |
| `update(sample, &fault)` | Memvalidasi sampel, mengevaluasi proteksi, dan mengembalikan fault latch saat ini. |
| `clear_fault(sample)` | Menghapus fault secara eksplisit setelah output mati dan kondisi pulih. |
| `get_fault()` | Membaca fault yang sedang dilatch. |
| `is_initialized()` | Memeriksa status lifecycle komponen. |
| `is_output_active()` | Membaca status output yang diberitahukan aplikasi. |

`update()` harus dipanggil serial dari satu task kontrol. Jangan memanggil
`update()`, `set_output_active()`, `clear_fault()`, atau `deinit()` secara
bersamaan dari beberapa task tanpa sinkronisasi eksternal.

## Konfigurasi

| Field | Default | Keterangan |
|---|---:|---|
| `over_voltage_enabled` | `true` | Aktifkan OVP; batas wajib diisi dengan nilai lebih dari nol. |
| `over_voltage_limit_v` | `0` | Ambang OVP dalam volt. Fault terjadi saat nilai `>=` ambang. |
| `over_voltage_hysteresis_v` | `0.2` | Margin di bawah batas yang harus dicapai untuk menghapus fault. |
| `over_current_enabled` | `true` | Aktifkan OCP; batas wajib diisi dengan nilai lebih dari nol. |
| `over_current_limit_a` | `0` | Ambang OCP dalam ampere. |
| `over_current_hysteresis_a` | `0.05` | Margin pemulihan OCP dalam ampere. |
| `over_power_enabled` | `false` | Aktifkan OPP bila pengukuran daya tersedia dan tervalidasi. |
| `over_power_limit_w` | `0` | Ambang OPP dalam watt. |
| `over_power_hysteresis_w` | `0.5` | Margin pemulihan OPP dalam watt. |
| `over_temperature_enabled` | `true` | Aktifkan OTP; batas wajib diisi dengan nilai lebih dari nol. |
| `over_temperature_limit_c` | `0` | Ambang OTP dalam derajat Celsius. |
| `over_temperature_hysteresis_c` | `2` | Margin pemulihan OTP dalam derajat Celsius. |
| `fault_gpio` | `GPIO_NUM_NC` | GPIO input opsional dari rangkaian fault eksternal. |
| `fault_gpio_active_high` | `true` | Level aktif untuk input fault GPIO. |
| `fault_gpio_pull_up` | `false` | Aktifkan pull-up internal jika sesuai rangkaian. |
| `fault_gpio_pull_down` | `true` | Aktifkan pull-down internal jika sesuai rangkaian. |
| `fault_gpio_debounce_samples` | `1` | Jumlah sampel GPIO aktif berurutan sebelum fault. |
| `*_debounce_samples` | `3` | Jumlah sampel berurutan di atas ambang sebelum OVP/OCP/OPP/OTP. |
| `startup_grace_period_ms` | `0` | Tunda evaluasi OVP/OCP/OPP setelah output diaktifkan. |
| `on_fault` | `nullptr` | Callback opsional yang dipanggil satu kali saat fault pertama dilatch. |
| `callback_context` | `nullptr` | Pointer konteks yang diteruskan ke callback. |

Setiap hysteresis harus finite, tidak negatif, dan lebih kecil dari ambang
proteksi terkait. Nilai limit harus finite dan lebih besar dari nol. Jika
proteksi dinonaktifkan, field sensor dan limit terkait tidak digunakan.

Debounce menghitung sampel berurutan, bukan waktu. Contohnya, tiga sampel
berturut-turut pada loop 10 ms berarti sekitar 30 ms; pada loop 100 ms berarti
sekitar 300 ms. Periode tersebut bergantung pada aplikasi.

Startup grace hanya menunda OVP, OCP, dan OPP. Grace period bukan pengganti
analisis inrush dan tidak menonaktifkan validasi sensor, OTP, atau input fault
GPIO. Gunakan nilai sesingkat yang dibutuhkan sistem.

## Jenis fault

| Fault | Arti |
|---|---|
| `None` | Tidak ada fault yang dilatch. |
| `OverVoltage` | Tegangan mencapai atau melampaui batas OVP selama jumlah sampel debounce. |
| `OverCurrent` | Arus mencapai atau melampaui batas OCP selama jumlah sampel debounce. |
| `OverPower` | Daya mencapai atau melampaui batas OPP selama jumlah sampel debounce. |
| `OverTemperature` | Suhu mencapai atau melampaui batas OTP selama jumlah sampel debounce. |
| `ShortCircuit` | Input fault GPIO aktif selama jumlah sampel debounce yang dikonfigurasi. |
| `SensorError` | Aplikasi menandai `sensor_error = true`. |
| `InvalidMeasurement` | Channel yang diproteksi tidak valid, non-finite, atau bernilai negatif untuk tegangan/arus/daya. |

## Batas keselamatan dan integrasi

- `Protection` tidak membuat task, membaca sensor, mengubah `DataShared`, atau
  mengendalikan GPIO enable PSU. Aplikasi bertanggung jawab menghubungkan
  callback ke fungsi shutdown output yang tepat.
- `set_output_active()` harus mencerminkan keadaan output sebenarnya. Panggil
  setelah output aktif/nonaktif berhasil diubah.
- Waktu deteksi ditentukan oleh periode sampling aplikasi, jumlah sampel
  debounce, dan startup grace bila digunakan. Mekanisme ini bukan interlock
  hardware atau jalur proteksi hard real-time.
- Jika callback `on_fault` gagal, fault tetap dilatch dan `update()` meneruskan
  error callback. Aplikasi harus menangani error tersebut secara konservatif
  dan menonaktifkan output melalui jalur independen bila memungkinkan.
- `fault_gpio` memerlukan rancangan level listrik yang jelas. Pull-up/pull-down
  internal mungkin tidak cukup untuk kabel panjang atau lingkungan bising;
  gunakan rangkaian eksternal sesuai kebutuhan.
- `clear_fault()` hanya menghapus latch software. Ia tidak menguji ulang
  hardware, menghapus fault pada rangkaian eksternal, atau mengaktifkan output.
- Untuk perangkat keselamatan-kritis, lakukan analisis risiko dan validasi
  perangkat keras tersendiri. Jangan mengandalkan firmware sebagai satu-satunya
  lapisan proteksi.

## Lisensi

Komponen ini didistribusikan di bawah lisensi MIT. Lihat [LICENSE](./LICENSE).
