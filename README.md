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
