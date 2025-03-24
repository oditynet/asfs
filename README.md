# asfs

Моя жопная файловая система самодостаточная и пока поддерживает:
1) Создание простого файла (не пмногострочного)
2) Модицикация,удаление файла
3) Создание и восстановление снапшота
4) Показ информации о системе хранения
5) Форматирование диска
....
```
Usage: ./asfs [options]
  -b <size>    Set block size (default 4096)
  -0           Zero fill device on format
  -f           Format device
  -c <f> <d>   Create file
  -l           List files
  -w           List snapshots
  -q <f>       Cat file
  -s <f> <n>   Create snapshot
  -r <f> <n>   Restore snapshot
  -e <f> <d>   Edit file
  -d <f>       Delete file
  -x <f>       Delete snapshot
  -p           Print FS info
```
# 23 - это новейшая файловая система записи с LRU L1 кэшем

Вот вам для сравнения генератор на ext4 1000 файлов по 256 байт:
```
 odity@viva  ~  bash testdisk.sh
Прогресс: [####################] 100%

Результаты:
--------------------------------
Создано файлов:   1000
Общий объем:      250 KB
Общее время:      .909995536 сек
Скорость:         1098.90 файлов/сек
Пропускная способность: .26 MB/s
Средний размер файла: 256 байт
--------------------------------
```
И скорость записи моей файловой системы (линейно)
```
./23 -f 20 -k 1024
Inode-X> benchmark
Benchmark results:
Total files:     1000
Total time:      0.022 seconds
Files per second: 46308.73
Throughput:      11.31 MB/s

Inode-X> exit
```
