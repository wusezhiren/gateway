# Paho 通过 ServiceLoader 加载 tcp/ssl/ws 网络模块，日志也按类名反射加载
-keep class org.eclipse.paho.client.mqttv3.** { *; }
-dontwarn org.eclipse.paho.client.mqttv3.**
