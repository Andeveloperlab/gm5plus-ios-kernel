Hedeflenen Mach-O ikilisinde veya XNU kaynak kodunda ARMv8.3-A Pointer Authentication (`pacibsp`, `autibsp`, `retaa`, `retab`, `blraa`) komutlarını tara.
Cortex-A53 (ARMv8.0-A) uyumluluğu için bunları `NOP` (`0x1F2003D5`) veya standart `RET` (`0xC0035FD6`) / `BLR` karşılıklarıyla değiştirecek minimal yamayı hazırla.
