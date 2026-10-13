@echo off
rem Эмулятор сервера для тестов на Windows. Параметры: py test_server.py --help
py "%~dp0test_server.py" %*
