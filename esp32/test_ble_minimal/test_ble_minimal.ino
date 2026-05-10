/*
 * 极简 BLE 可见性测试 - ESP32-C3 (名称修复版)
 * 确保设备名称“TestBLE”能被手机扫描到
 */

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEAdvertising.h>

void setup() {
  Serial.begin(115200);
  delay(1500);  // 等待串口完全稳定
  Serial.println("\n\n=============================");
  Serial.println(" BLE 极简测试启动 (名称修复)");
  Serial.println("=============================");

  Serial.print("芯片: "); Serial.println(ESP.getChipModel());
  Serial.print("堆:  "); Serial.println(ESP.getFreeHeap());

  // 初始化 BLE
  Serial.println("[1] 初始化 NimBLE...");
  NimBLEDevice::init("TestBLE");
  Serial.println("    初始化完成");

  // 创建服务器（必须，否则无法广播）
  NimBLEServer* pServer = NimBLEDevice::createServer();
  if (!pServer) {
    Serial.println("    错误：创建服务器失败");
    return;
  }
  Serial.println("[2] 服务器已创建");

  // 获取广播对象
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  if (!pAdv) {
    Serial.println("    错误：获取广播对象失败");
    return;
  }

  // ---- 关键：设置广播数据 ----
  pAdv->setName("TestBLE");         // 设备名称（必填！）
  pAdv->setAppearance(0x03C0);      // Generic HID
  pAdv->setMinInterval(32);
  pAdv->setMaxInterval(48);

  // 启动广播
  bool ok = pAdv->start();
  if (ok) {
    Serial.println("[3] ✓✓✓ 广播已启动！");
    Serial.println("    设备名称: TestBLE");
    Serial.println("    请用手机蓝牙扫描查找 'TestBLE'");
    Serial.println("    如果仍然搜不到，请将手机靠近板子天线区域");
  } else {
    Serial.println("[3] ✗ 广播启动失败！");
  }

  Serial.printf("当前堆剩余: %d 字节\n", ESP.getFreeHeap());
  Serial.println("=============================");
}

void loop() {
  delay(5000);
  Serial.printf("运行中… 堆: %d\n", ESP.getFreeHeap());
}
