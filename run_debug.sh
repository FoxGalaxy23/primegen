./my_browser > debug_stdout.log 2> debug_stderr.log &
B_PID=$!
sleep 5
kill -SIGTERM $B_PID
