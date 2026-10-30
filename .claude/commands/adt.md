Yerel `msm8952-shamrock.dtb` / `.dtsi` içindeki donanım adreslerini (BLSP UART, GICv2 `0xb000000`, RAM bölgeleri) kontrol et.
Asla adres uydurma! Apple Device Tree (`chosen`, `arm-io`) düğümlerini oluştururken yalnızca surgical diff (`diff -u`) formatında yama üret.
