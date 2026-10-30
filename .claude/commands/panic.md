Son oluşan QEMU/UART log dosyasının yalnızca son 40 satırını oku. Tüm dosyayı bağlama alma!
Sadece `panic()`, `ESR_EL1`, `FAR_EL1`, `PC`, `LR` değerlerini çıkar.
ESR_EL1 sendrom bitlerini (EC ve ISS) ARMv8.0-A Cortex-A53 referansına göre çözümle ve çökmenin kök nedenini (Alignment fault, Translation fault, Undefined instruction / PAC vb.) tek paragrafta raporla.
