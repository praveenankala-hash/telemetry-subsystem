# 1. Fire up your user app in the background, keeping standard output open
sudo ../bin/telemetry_user &
USER_APP_PID=$!

# 2. Run the network burst engine in the foreground
for i in {1..10}; do 
    curl -s https://www.google.com > /dev/null
    echo "Packet Burst $i fired"
done

# 3. Kill the background thread handler
#sudo kill $USER_APP_PID
time sudo ../bin/telemetry_user &

# 4. View the unified interleaved buffer contents
sudo head -c 1024 /dev/telemetry | hexdump -C
