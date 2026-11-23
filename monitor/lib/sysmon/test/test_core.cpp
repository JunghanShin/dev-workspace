// 유닛 시험(코어만, libworker 없음): tegrastats 줄(JetPack 판별 표기) → 지표 이름·값·글자, 가짜 /proc·/sys 나무 → 추가 지표.
// 기대값은 옛 Python 판(sysmon.py parse·SystemExtras)이 같은 입력에 내던 것
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include "sysmon/extras.hpp"
#include "sysmon/metrics.hpp"
#include "sysmon/tegrastats.hpp"

using namespace sysmon;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)
#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const std::string a_ = (a), b_ = (b);                                                            \
        if (a_ != b_) {                                                                                  \
            std::printf("FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, __LINE__, #a, b_.c_str(), a_.c_str()); \
            ++failures;                                                                                  \
        }                                                                                                \
    } while (0)

static std::string get(const MetricList& m, const std::string& k) {
    const Metric* x = m.find(k);
    return x ? x->text : "<none>";
}

// 기대 목록과 정확히 같은지(이름·글자·순서)
static void expect_all(const MetricList& m, const std::string& want) {
    CHECK_EQ(m.str(), want);
}

static void write(const std::string& path, const std::string& text) {
    std::ofstream(path) << text;
}

static void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < path.size()) cur += path[i];
    }
}

static void test_metric_key_and_float() {
    CHECK_EQ(metric_key("VDD_GPU_SOC"), "vdd_gpu_soc");
    CHECK_EQ(metric_key("NVDLA0"), "nvdla0");
    CHECK_EQ(metric_key("_a--B__c_"), "a_b_c");
    CHECK_EQ(metric_key("soc2"), "soc2");
    CHECK_EQ(py_float(45.0), "45.0");
    CHECK_EQ(py_float(46.937), "46.937");
    CHECK_EQ(py_float(-0.5), "-0.5");
    CHECK_EQ(py_float(0.1 + 0.2), "0.30000000000000004");
    MetricList m;
    m.set_round1("a", 12.25);  // Python round(12.25, 1) = 12.2 (딱 절반은 짝수 쪽)
    m.set_round1("b", 16.0 / 3);
    m.set_round1("c", 7.0);
    m.set_int("d", 3);
    m.set_int("a", 9);  // 같은 키: 자리는 그대로
    expect_all(m, "a=9 b=5.3 c=7.0 d=3");
}

// JetPack 4.x (Nano/TX2/Xavier 옛 표기): EMC/GR3D @MHz, NVENC 숫자, NVDEC off, VIC_FREQ %@, 온도 정수, 전력 mW 없음
static void test_jp4() {
    const auto m = parse_tegrastats(
        "RAM 2034/3964MB (lfb 4x4MB) SWAP 0/1982MB (cached 0MB) IRAM 0/252kB(lfb 252kB) "
        "CPU [5%@1479,2%@1479,4%@1479,1%@1479] EMC_FREQ 3%@1600 GR3D_FREQ 9%@921 NVENC 1036 NVDEC off VIC_FREQ 0%@192 APE 25 "
        "PLL@36C CPU@38.5C PMIC@50C GPU@36C AO@44.5C thermal@37.5C POM_5V_IN 3373/3373 POM_5V_GPU 0/0 POM_5V_CPU 413/413\n");
    expect_all(m,
               "ram_used_mb=2034 ram_total_mb=3964 swap_used_mb=0 swap_total_mb=1982 "
               "cpu0_pct=5 cpu0_mhz=1479 cpu1_pct=2 cpu1_mhz=1479 cpu2_pct=4 cpu2_mhz=1479 cpu3_pct=1 cpu3_mhz=1479 "
               "cpu_online=4 cpu_avg_pct=3.0 cpu_max_pct=5 cpu_mhz_max=1479 emc_pct=3 gpu_pct=9 gpu_mhz=921 "
               "nvenc_mhz=1036 nvdec_mhz=0 vic_mhz=192 "
               "temp_pll_c=36.0 temp_cpu_c=38.5 temp_pmic_c=50.0 temp_gpu_c=36.0 temp_ao_c=44.5 temp_thermal_c=37.5 temp_max_c=50.0");
}

// JetPack 5.x (Xavier NX/AGX Orin, root): 꺼진 코어 off, GR3D_FREQ %@[MHz], NVDLA0_FREQ @MHz, VIC off, -256C 센서 없음, 전력 mW/mW
static void test_jp5() {
    const auto m = parse_tegrastats(
        "11-23-2026 10:00:00 RAM 5102/30536MB (lfb 6233x4MB) SWAP 0/15268MB (cached 0MB) "
        "CPU [1%@729,off,off,off,0%@729,0%@729,0%@729,0%@729,off,off,off,off] EMC_FREQ 0%@2133 GR3D_FREQ 0%@[0,0] "
        "NVENC off NVDEC off NVJPG off NVJPG1 off VIC off OFA off NVDLA0 off NVDLA1 off PVA0_FREQ off APE 174 "
        "CV0@-256C CPU@46.281C Tboard@35C SOC2@42.75C Tdiode@36.75C SOC0@43.218C CV1@-256C GPU@-256C tj@46.281C SOC1@42.656C CV2@-256C "
        "VDD_GPU_SOC 2386mW/2386mW VDD_CPU_CV 397mW/397mW VIN_SR 1100mW/1100mW VIN_SYS_5V0 2212mW/2212mW VDDQ_VDD2_1V8AO 302mW/302mW");
    CHECK_EQ(get(m, "cpu0_pct"), "1");
    CHECK_EQ(get(m, "cpu1_pct"), "<none>");
    CHECK_EQ(get(m, "cpu4_mhz"), "729");
    CHECK_EQ(get(m, "cpu8_pct"), "<none>");
    CHECK_EQ(get(m, "cpu_online"), "5");
    CHECK_EQ(get(m, "cpu_avg_pct"), "0.2");
    CHECK_EQ(get(m, "cpu_max_pct"), "1");
    CHECK_EQ(get(m, "emc_pct"), "0");
    CHECK_EQ(get(m, "gpu_pct"), "0");
    CHECK_EQ(get(m, "gpu_mhz"), "0");  // [0,0]의 첫 값
    for (const char* k : {"nvenc_mhz", "nvdec_mhz", "nvjpg_mhz", "vic_mhz", "ofa_mhz", "nvdla0_mhz", "nvdla1_mhz"}) CHECK_EQ(get(m, k), "0");
    CHECK_EQ(get(m, "nvjpg1_mhz"), "<none>");  // NVJPG1은 옛 판 정규식 밖
    CHECK_EQ(get(m, "temp_cv0_c"), "<none>");
    CHECK_EQ(get(m, "temp_gpu_c"), "<none>");
    CHECK_EQ(get(m, "temp_cpu_c"), "46.281");
    CHECK_EQ(get(m, "temp_tboard_c"), "35.0");
    CHECK_EQ(get(m, "temp_soc2_c"), "42.75");
    CHECK_EQ(get(m, "temp_max_c"), "46.281");
    CHECK_EQ(get(m, "power_vdd_gpu_soc_mw"), "2386");
    CHECK_EQ(get(m, "power_vdd_cpu_cv_mw"), "397");
    CHECK_EQ(get(m, "power_vin_sr_mw"), "1100");
    CHECK_EQ(get(m, "power_vin_sys_5v0_mw"), "2212");
    CHECK_EQ(get(m, "power_vddq_vdd2_1v8ao_mw"), "<none>");  // VDDQ_는 옛 판 정규식((VDD|VIN)_) 밖
}

// JetPack 6.x (Orin Nano/NX): 소문자 온도 이름, GR3D_FREQ 정수 %(클럭 없음), VDD_IN, NVDLA0_FREQ @[MHz], VIC_FREQ %@
static void test_jp6() {
    const auto m = parse_tegrastats(
        "11-23-2026 10:00:01 RAM 2611/7620MB (lfb 2x4MB) SWAP 0/3810MB (cached 0MB) CPU [2%@729,0%@729,1%@729,0%@729,0%@729,4%@729] "
        "EMC_FREQ 1%@2133 GR3D_FREQ 0% NVENC 1036 NVDEC 0%@115 NVDLA0_FREQ @[1600] NVDLA1_FREQ @1600 OFA_FREQ @408 VIC_FREQ 0%@115 "
        "cpu@46.937C soc2@44.281C soc0@45.093C gpu@44.718C tj@46.937C soc1@44.875C "
        "VDD_IN 4524mW/4524mW VDD_CPU_GPU_CV 486mW/486mW VDD_SOC 1458mW/1458mW\n");
    expect_all(m,
               "ram_used_mb=2611 ram_total_mb=7620 swap_used_mb=0 swap_total_mb=3810 "
               "cpu0_pct=2 cpu0_mhz=729 cpu1_pct=0 cpu1_mhz=729 cpu2_pct=1 cpu2_mhz=729 cpu3_pct=0 cpu3_mhz=729 "
               "cpu4_pct=0 cpu4_mhz=729 cpu5_pct=4 cpu5_mhz=729 cpu_online=6 cpu_avg_pct=1.2 cpu_max_pct=4 cpu_mhz_max=729 "
               "emc_pct=1 gpu_pct=0 nvenc_mhz=1036 nvdec_mhz=115 nvdla0_mhz=1600 nvdla1_mhz=1600 ofa_mhz=408 vic_mhz=115 "
               "temp_cpu_c=46.937 temp_soc2_c=44.281 temp_soc0_c=45.093 temp_gpu_c=44.718 temp_tj_c=46.937 temp_soc1_c=44.875 "
               "temp_max_c=46.937 power_vdd_in_mw=4524 power_vdd_cpu_gpu_cv_mw=486 power_vdd_soc_mw=1458");
}

// 일반 계정 실행(EMC·전용 HW 없음), 같은 센서 이름 두 번(나중 값, 자리는 처음), 알아볼 수 없는 줄
static void test_partial_and_garbage() {
    const auto m = parse_tegrastats("RAM 100/200MB CPU [off,off] AO@40C AO@41.5C BAD@-256C");
    expect_all(m, "ram_used_mb=100 ram_total_mb=200 temp_ao_c=41.5 temp_max_c=41.5");  // 코어가 다 꺼지면 cpu_* 없음
    CHECK(parse_tegrastats("hello from a strange tegrastats").empty());
    CHECK(parse_tegrastats("").empty());
    CHECK(parse_tegrastats("GPU@-256C").empty());  // 센서 없음뿐이면 빈 목록
    const auto g = parse_tegrastats("GR3D_FREQ 63%@[1300,1300]");
    expect_all(g, "gpu_pct=63 gpu_mhz=1300");
}

// 가짜 /proc·/sys 나무
static void test_extras() {
    char tmpl[] = "/tmp/sysmon_core_XXXXXX";
    const std::string root = mkdtemp(tmpl);
    mkdirs(root + "/proc/net");
    mkdirs(root + "/sys/class/hwmon/hwmon0");
    mkdirs(root + "/sys/class/hwmon/hwmon3");
    mkdirs(root + "/sys/class/hwmon/hwmon5");
    mkdirs(root + "/sys/class/net/eth0/statistics");
    mkdirs(root + "/sys/class/net/wlan0/statistics");
    mkdirs(root + "/var/lib/nvpmodel");
    write(root + "/sys/class/hwmon/hwmon0/name", "thermal\n");
    write(root + "/sys/class/hwmon/hwmon3/name", "pwmfan\n");
    write(root + "/sys/class/hwmon/hwmon3/pwm1", "128\n");
    write(root + "/sys/class/hwmon/hwmon5/name", "pwm_tach\n");
    write(root + "/sys/class/hwmon/hwmon5/rpm", "2412\n");
    // 기본 경로 둘(eth0 metric 100, wlan0 metric 600) + 기본 아닌 경로 + 꺼진(UP 아님) 기본 경로
    write(root + "/proc/net/route",
          "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
          "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
          "eth0\t00000000\t0100A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
          "eth0\t0000A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
          "usb0\t00000000\t0102A8C0\t0002\t0\t0\t1\t00000000\t0\t0\t0\n");
    write(root + "/sys/class/net/eth0/statistics/rx_bytes", "1000000\n");
    write(root + "/sys/class/net/eth0/statistics/tx_bytes", "500000\n");
    write(root + "/proc/uptime", "12345.67 40000.00\n");
    write(root + "/var/lib/nvpmodel/status", "pmode:0002\n");

    CHECK_EQ(default_iface(root), "eth0");
    CHECK(read_power_mode(root) == 2);
    CHECK(!read_power_mode(root + "/nowhere"));

    SystemExtras ex(root);
    CHECK_EQ(ex.fan_pwm_path(), root + "/sys/class/hwmon/hwmon3/pwm1");
    CHECK_EQ(ex.fan_rpm_path(), root + "/sys/class/hwmon/hwmon5/rpm");
    const auto a = ex.sample(100.0);
    CHECK(a.find("disk_total_gb") && a.find("disk_used_gb"));
    CHECK(!a.find("net_rx_kbps"));  // 첫 표본은 속도 없음
    CHECK_EQ(get(a, "fan_pct"), "50");  // round(128 * 100 / 255) = 50
    CHECK_EQ(get(a, "fan_rpm"), "2412");
    CHECK_EQ(get(a, "uptime_s"), "12345");
    // 2 s 뒤 rx +250000 B, tx +12345 B → 1000.0 kbps, 49.4 kbps
    write(root + "/sys/class/net/eth0/statistics/rx_bytes", "1250000\n");
    write(root + "/sys/class/net/eth0/statistics/tx_bytes", "512345\n");
    write(root + "/sys/class/hwmon/hwmon3/pwm1", "255\n");
    write(root + "/sys/class/hwmon/hwmon5/rpm", "-1\n");
    const auto b = ex.sample(102.0);
    CHECK_EQ(get(b, "net_rx_kbps"), "1000.0");
    CHECK_EQ(get(b, "net_tx_kbps"), "49.4");
    CHECK_EQ(get(b, "fan_pct"), "100");
    CHECK_EQ(get(b, "fan_rpm"), "-1");
    // 순서: disk → net → fan → uptime (옛 판 dict 순서)
    std::string keys;
    for (const auto& x : b.items()) keys += x.key + " ";
    CHECK_EQ(keys, "disk_total_gb disk_used_gb net_rx_kbps net_tx_kbps fan_pct fan_rpm uptime_s ");
    // 읽지 못하는 값은 빠진다
    write(root + "/sys/class/hwmon/hwmon3/pwm1", "x\n");
    write(root + "/sys/class/net/eth0/statistics/rx_bytes", "");
    const auto c = ex.sample(103.0);
    CHECK(!c.find("fan_pct"));
    CHECK(!c.find("net_rx_kbps"));
    // 기본 경로 인터페이스가 바뀌면 그 표본은 속도 없음
    write(root + "/sys/class/net/eth0/statistics/rx_bytes", "1300000\n");
    ex.sample(104.0);
    write(root + "/proc/net/route",
          "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
          "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
    write(root + "/sys/class/net/wlan0/statistics/rx_bytes", "10\n");
    write(root + "/sys/class/net/wlan0/statistics/tx_bytes", "10\n");
    CHECK(!ex.sample(105.0).find("net_rx_kbps"));
    CHECK(ex.sample(106.0).find("net_rx_kbps"));
    // 장치가 하나도 없는 나무
    SystemExtras none(root + "/nowhere", root + "/nowhere");
    const auto d = none.sample(1.0);
    CHECK(d.empty());
    (void)!std::system(("rm -rf " + root).c_str());
}

int main() {
    test_metric_key_and_float();
    test_jp4();
    test_jp5();
    test_jp6();
    test_partial_and_garbage();
    test_extras();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("sysmon core: all ok\n");
    return failures ? 1 : 0;
}
