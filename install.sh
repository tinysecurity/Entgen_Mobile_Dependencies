#!/bin/bash

SERVICE="myservice.service"

# Enable service
sudo systemctl enable $SERVICE

# Start service
sudo systemctl start $SERVICE

if systemctl -q is-active $SERVICE
then
    echo "$SERVICE is up and running!"
else
    echo "Failed to start $SERVICE."
fi