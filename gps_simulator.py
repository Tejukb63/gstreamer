import time
import serial

# CHANGE THIS to the first port socat gave you
SERIAL_PORT = "/dev/pts/5" 

print(f"Starting simulated GNSS on {SERIAL_PORT}...")
ser = serial.Serial(SERIAL_PORT, 9600)

lat = 12.9716
lon = 77.5946

try:
    while True:
        # Generate a fake standard NMEA GGA sentence
        nmea = f"$GPGGA,123519.00,{(lat):02.4f},N,{(lon):03.4f},E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
        ser.write(nmea.encode("ascii"))
        print(f"Broadcasting: {nmea.strip()}")

        lat += 0.0001
        lon += 0.0001
        time.sleep(1)
except KeyboardInterrupt:
    ser.close()
    
    
    
    
    #socat -d -d pty,raw,echo=0 pty,raw,echo=0

