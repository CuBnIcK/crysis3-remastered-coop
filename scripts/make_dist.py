"""Assemble dist/c3coop/: the folder friends copy to their PC (launcher, DLL, data pak, README.txt in Russian).

  python scripts/make_dist.py [--zip]     (run build.bat and scripts/build_pak.py first)

--zip also writes dist/c3coop.zip with the same folder inside.
"""
import argparse
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
DIST = os.path.join(ROOT, 'dist', 'c3coop')
FILES = ['c3coop_launch.exe', 'c3coop.dll', 'c3coop.pak']

README = r"""c3coop - кооператив для кампании Crysis 3 Remastered (Steam)
===========================================================

Что это
-------
Мод позволяет пройти одиночную кампанию вдвоём-вчетвером. Тот, кто начинает
миссию, становится хостом; друзья подключаются к нему через
Steam (приглашение или "Присоединиться к игре" в списке друзей). Проброс
портов не нужен: трафик игры идёт через сеть Steam.
Мод должен стоять у ВСЕХ игроков, и версия должна быть одинаковой.

Установка (один раз)
--------------------
1. Скопируйте папку c3coop в любое место, куда у вас есть право записи,
   например C:\Games\c3coop (не в Program Files). Мод пишет логи
   в подпапки logs, crash, inject рядом с собой.
2. Steam -> Библиотека -> Crysis 3 Remastered -> правой кнопкой -> Свойства
   -> Общие -> Параметры запуска. Вставьте (путь замените на свой, кавычки
   обязательны):

       "C:\Games\c3coop\c3coop_launch.exe" %command%

3. Оверлей Steam должен быть включён (Свойства -> Общие -> "Включить
   оверлей Steam в игре") - через него отправляются приглашения.

Игра
----
Запускайте игру как обычно, кнопкой "Играть" в Steam.

Хост:
- Начните новую кампанию или выберите миссию в меню выбора миссий.
  "Продолжить" (загрузка сохранения) пока не работает в коопе: такая игра
  остаётся одиночной. Через пару секунд после загрузки уровня мод сам
  создаёт лобби Steam только для друзей (до 4 игроков).
- Пригласите друзей: Shift+Tab -> друзья -> правой кнопкой по другу ->
  "Пригласить в игру". Или откройте консоль (клавиша ~) и введите:
      c3coop steam invite
- Друзья также видят у вас в списке друзей кнопку "Присоединиться к игре".

Друг:
- Примите приглашение в Steam или нажмите "Присоединиться к игре" у хоста.
  Если игра не запущена, Steam запустит её сам (через параметры запуска с
  модом) и подключение начнётся примерно через 10 секунд после старта.
- Если вы в это время были в своём уровне, мод сначала выйдет из него.
- Когда хост переходит на следующий уровень, клиенты переподключаются сами.
- Не вводите сами "connect" и "map": подключением занимается мод. Ручной
  connect параллельно с подключением через Steam сбивает обе попытки.

Особенности коопа:
- Двери и проходы "в одну сторону" ждут команду: когда внутри все игроки,
  кроме одного (3 из 4, 2 из 3), остальных переносит к ним. На экране
  пишется, сколько игроков ещё ждут.
- Погибший игрок возвращается рядом с живым напарником.
- Улучшения костюма каждый игрок подбирает себе сам.
- Отстали или застряли - введите в консоли c3coop tp (к ближайшему
  напарнику) или c3coop tp host (к хосту).

Команды консоли (клавиша ~)
---------------------------
  c3coop steam status        состояние Steam, лобби, игроки, трафик
  c3coop steam invite        окно приглашения друзей
  c3coop steam join <id>     подключиться к хосту по SteamID64 (или к лобби по id)
  c3coop steam leave         выйти из лобби и отключить ретранслятор
  c3coop steam host          создать лобби вручную, если оно не создалось
  c3coop steam autohost off  не создавать лобби автоматически (on - вернуть)
  c3coop tp [host|<имя>]     телепорт к ближайшему напарнику / к хосту / к игроку

Если что-то не работает
-----------------------
- c3coop_launch.log рядом с c3coop_launch.exe: запуск игры и загрузка мода.
- logs\<номер процесса>.log: журнал мода (строки "steam: ..." - лобби и
  подключение). Пришлите этот файл хосту/автору мода.
- Если в c3coop_launch.log нет записи о запуске, проверьте параметры
  запуска (кавычки, путь, слово %command% в конце).
- Без параметра запуска игра запускается без мода (обычная одиночная игра).
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zip', action='store_true', help='also write dist/c3coop.zip')
    args = ap.parse_args()

    missing = [f for f in FILES if not os.path.exists(os.path.join(BUILD, f))]
    if missing:
        sys.exit('missing in build/: ' + ', '.join(missing) + ' (run build.bat and scripts/build_pak.py)')
    if os.path.isdir(DIST):
        shutil.rmtree(DIST)
    os.makedirs(DIST)
    for f in FILES:
        shutil.copy2(os.path.join(BUILD, f), os.path.join(DIST, f))
    # UTF-8 with BOM and CRLF so old Notepad versions show Cyrillic correctly
    with open(os.path.join(DIST, 'README.txt'), 'w', encoding='utf-8-sig', newline='\r\n') as f:
        f.write(README.lstrip('\n'))
    for f in sorted(os.listdir(DIST)):
        print(f'{os.path.join(DIST, f)}  {os.path.getsize(os.path.join(DIST, f)) / 1024:.0f} KB')

    if args.zip:
        out = os.path.join(os.path.dirname(DIST), 'c3coop.zip')
        with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
            for f in sorted(os.listdir(DIST)):
                z.write(os.path.join(DIST, f), f'c3coop/{f}')
        print(f'wrote {out}: {os.path.getsize(out) / 1024:.0f} KB')


if __name__ == '__main__':
    main()
