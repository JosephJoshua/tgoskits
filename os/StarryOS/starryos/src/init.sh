#!/bin/sh

export HOME=/root

echo -e "Welcome to \e[96m\e[1mStarry OS\e[0m!"
env
echo

echo -e "Use \e[1m\e[3mapk\e[0m to install packages."
echo

# If test runner exists, run tests and power off instead of starting shell
if [ -x /test_runner.sh ]; then
    /test_runner.sh
    exit $?
fi

# Do your initialization here!

cd ~
sh --login
