import numpy as np
import quaternion
import time
import math
import viser
import serial
import socket
import threading
from datetime import datetime

from zeroconf import IPVersion, ServiceInfo, Zeroconf

from geom import ZoneAngles, compute_zone_angles

# Class for "old" usb-serial-based protocol
class serialParser:
    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self._thread: threading.Thread | None = None
        self._running=False
        self._data_lock = threading.Lock() 
        self._distances=[]
        for i in range(0, 8):
            self._distances.append([])
            for j in range(0, 64):
                self._distances[i].append(0)

    def get_distances(self, sensor_num):
        with self._data_lock:
            return self._distances[sensor_num].copy()
    def connect(self):
        print(f"Connecting to {self.port} @ {self.baud} baud")
        self.serial = serial.Serial(port=self.port, baudrate=self.baud, timeout=1) 
        time.sleep(2)
        self.serial.reset_input_buffer()
    def start(self):
        self._running=True
        self._thread= threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()
    def stop(self):
        self._running=False
        if self.serial:
            self.serial.close()
        if self._thread:
            self._thread.join(timeout=1)
            self._thread=None
    def _read_loop(self):
        while self._running:
            try:
                line = self.serial.readline().decode("utf-8", errors="ignore").strip()
                if line.startswith("<> "):
                    l = line.split(" ")
                    snum = int(l[1])
                    print(snum, end=" ")
                    for m in range(2, len(l)):
                        self._distances[snum][m-2] = int(l[m])
                else:
                    print(f"\nesp> {line}");
            except:
                pass

# class for final network-based protocol (literally just a TCP port)
class networkParser:
    def __init__(self, port):
        self.sock = socket.socket()
        self.port = port
        self._thread: threading.Thread | None = None
        self._running=False
        self._data_lock = threading.Lock() 
        self._distances=[]
        self._buffer=""
        self._file=""
        self._dbgdata = {
                "lsm6_temp": -1,
                "lis2_temp": -1,
                "esp_temp": -1
                }
        for i in range(0, 8):
            self._distances.append([])
            for j in range(0, 64):
                self._distances[i].append(0)
    # From  https://stackoverflow.com/a/28950776
    # The tricky part here is that getting localhost's IP address
    # returns 127.0.0.1 (which is, by definition, correct)
    # However, I care about what my 'local' IP address is from
    # the perspective of other devices behind the same NAT as this machine
    # So, we have to do this hack-ish solution.
    # This will only work if the esp32 and this program are running on the same
    # "third party" network. If this program is running on a laptop hotspot, this
    # solution will probably break, as this function would return the IP address 
    # of the interface used to connect to the internet, while the esp32 would be on
    # a different subnet created by the wifi hotspot driver
    def _get_ip(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0)
        try:
            # doesn't even have to be reachable
            s.connect(('10.254.254.254', 1))
            IP = s.getsockname()[0]
        except Exception:
            IP = '127.0.0.1'
        finally:
            s.close()
        return IP
    def get_distances(self, sensor_num):
        with self._data_lock:
            return self._distances[sensor_num].copy()
    def getDbg(self):
        with self._data_lock:
            return self._dbgdata
    def start(self):
        self._running=True
        self._thread= threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()
    def stop(self):
        self._running=False
        if self._thread:
            self._thread.join(timeout=1)
            self._thread=None
    def _handlePacket(self, packet):
        #print(packet)
        if packet.startswith("<>"):
            parts = packet.strip().split(" ")
            if parts[1] == "S":
                print(f"@{parts[2]}: FPS: {1/(int(parts[3])/1000000)}")
            # Check if sensor num
            elif parts[1] in ["0", "1", "2", "3", "4", "5", "6", "7"]:
                snum = int(parts[1]) #we need to go from Firmwares sensor (0-7) to visers sensors (1-8)
                with self._data_lock:
                    for m in range(3, len(parts)):
                        val = int(parts[m])
                        self._distances[snum][m-3] = val
                #print(f"@{parts[2]}: Sensor {snum}")
                #print("Failed to parse sensor packet")
            elif parts[1] == "A":
                # Accelerometer message
                pass
            elif parts[1] == "G":
                # Gyro message
                pass
            elif parts[1] == "M":
                # Magnetometer message
                pass
            elif parts[1] == "T":
                #Temperature message
                with self._data_lock:
                    self._dbgdata["lsm6_temp"] = float(parts[3])
            else:
                print(f"Got unknown packet type {parts[1]}")
        else:
            print(f"invalid packet: {packet}")
    def _handleUpdateBuffer(self, data):
        append = data.decode("utf8")
        if "\n" in append:
            inbetween = append.split("\n")
            self._buffer+=inbetween[0]
            self._handlePacket(self._buffer)
            self._buffer=inbetween[1]
        else:
            self._buffer+=append
    def _read_loop(self):
        print("Registering mDNS service")
        info = ServiceInfo(
            "_lwLIDAR._tcp.local.",
            "_Offloader._lwLIDAR._tcp.local.",
            addresses=[self._get_ip()],
            port=3133,
            #properties=desc,
            server="lwLIDAR_offload.local.",
        )
        zeroconf = Zeroconf(ip_version=IPVersion.All)
        zeroconf.register_service(info)

        bind_failed = True
        while bind_failed:
            try:
                self.sock.bind(('0.0.0.0', 3133))
                self.sock.listen(0)
                break
            except OSError:
                print("Address still in use. Waiting 10s")
                time.sleep(10)

        while self._running:
            filename = datetime.now().strftime("test-%Y-%m-%d_%H_%M_%S.log")
            with open(filename, "a") as self._file:
                print(f"Writing sensor data to: {filename}");
                while self._running:
                    try:
                        print("Waiting for connection")
                        #self.sock.settimeout(3)
                        client, addr = self.sock.accept()
                        print("Accept connection")
                        while self._running:
                            content = client.recv(128)
                            txt = content.decode("utf-8");
                            #print(txt, end="")
                            self._file.write(txt)
                            # NOTE: this is not ideal for performance (in theory), but in practice I have
                            # so much file write bandwidth on my laptop that I'd much rather be able to tail
                            # the logfile and see "live" updates than care abour file IO efficiency
                            self._file.flush()
                            if len(content) == 0:
                                    break
                            else:
                                self._handleUpdateBuffer(content)
                    except TimeoutError:
                        print("Socket connection timed out")
                    finally:
                        self.sock.close()
        print("Closing socket")
        self.sock.close()
            


# "test pattern" for me to check if sensor frames are correct
def get_sample_distances(sensor_num):
    a = [
            900, 100, 100, 100, 100, 100, 100, 100,
            100, 900, 100, 100, 100, 100, 100, 100,
            100, 100, 900, 100, 100, 100, 100, 100,
            100, 100, 100, 900, 100, 100, 100, 100,
            100, 100, 100, 100, 900, 100, 100, 100,
            100, 100, 100, 100, 100, 900, 100, 100,
            100, 100, 100, 100, 100, 100, 900, 100,
            100, 100, 100, 100, 100, 100, 100, 900
        ]
    return a


# function to take array of points and output part of point cloud
def gen_points(server, zone_angles, distances, sensor_num):
    #TODO make transformation matrix to apply to points at end of calculation
    #TODO for each point in 2d array calculate position relative to the sensor origin from the lookup table
    min_range = 20 / 1000
    max_range = 4000 / 1000
    
    rays = np.zeros((64, 2, 3))
    points = np.zeros((64, 3))
    for i in range(64):
        # Use ST lookup ray directions (already normalized)
        dir_x = zone_angles.st_ray_dir_x[i]
        dir_y = zone_angles.st_ray_dir_y[i]
        dir_z = zone_angles.st_ray_dir_z[i]
        # Scale to match tangent-style (z=1 convention)
        if dir_z > 0:
            dir_x = dir_x / dir_z
            dir_y = dir_y / dir_z
            dir_z = 1.0

        # Use measured distance if provided and valid, otherwise max range
        if distances is not None and distances[i] >= 20:
            end_range = distances[i] / 1000
        else:
            # Because we're using negative numbers for invalid measurement return codes.
            end_range = 0
        start = np.array([
            min_range * dir_x,
            min_range * dir_y,
            min_range * dir_z,
        ], dtype=np.float32)
        end = np.array([
            end_range * dir_x,
            end_range * dir_y,
            end_range * dir_z,
        ], dtype=np.float32)
        rays[i] = [start, end]
        points[i] = end

    npdistances = np.array(distances)
    normalised_dists = (npdistances - 20) / (3500-20)
    colours = np.zeros((64, 3), dtype=np.uint8);
    colours[:, 0] = ((1-normalised_dists) * 255).astype(np.uint8)
    colours[:, 1] = (normalised_dists * 255).astype(np.uint8)


    ray = server.scene.add_line_segments(
        f"/sensors/{sensor_num}/rays",
        points=rays,
        colors=(100, 150, 255),
        visible=False
    )
    pcloud = server.scene.add_point_cloud(
        f"/sensors/{sensor_num}/points",
        points=points,
        colors=colours,#(0, 255, 0),
        point_size=0.03,
        point_shape="circle"
    )


#Render sensor data
def process_frame(server, zone_angles, sport, lsm6_temp, lis2_temp, esp_temp):
    #get 8 sensors of data from parser
    for sensor_num in range(1, 9):
        gen_points(server, zone_angles, sport.get_distances(sensor_num-1), sensor_num)
    dbgdata = sport.getDbg()
    #TODO use a "proper" invalid value, as -1 could technically be a valid temperature
    if dbgdata["lsm6_temp"] != -1:
        lsm6_temp.value = f"{round(dbgdata["lsm6_temp"], 1)} °C"
    if dbgdata["lis2_temp"] != -1:
        lis2_temp.value = f"{round(dbgdata["lis2_temp"], 1)} °C"
    if dbgdata["esp_temp"] != -1:
        esp_temp.value = f"{round(dbgdata["esp_temp"])} °C"

#the mainloop thing
def main():
    sport = networkParser(3133)
    sport.start()
    # set up viser server
    server = viser.ViserServer()
    # add sensor frames
    server.scene.add_frame(
            "/sensors/1",
            wxyz=(0.653, 0.271, -0.653, 0.271), #8
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/2",
            wxyz=(0.5, 0.5, -0.5, 0.5), #7
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/3",
            wxyz=(0.271, 0.653, -0.271, 0.653), #6
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/4",
            wxyz=(0, -0.707, -0, -0.707), #5
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/5",
            wxyz=(0.271, -0.653, -0.271, -0.653), #4
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/6",
            wxyz=(0.5, -0.5, -0.5, -0.5), #3
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/7",
            wxyz=(0.635, -0.271, -0.653, -0.271), #2
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    server.scene.add_frame(
            "/sensors/8",
            wxyz=(0.707, 0, -0.707, -0), #1
            position=(0, 0, 0), #TODO add offset
            show_axes=False
        )
    with server.gui.add_folder("Debug data"):
        lsm6_temp = server.gui.add_text(
                "lsm6_temp",
                initial_value="No Data"
                )
        lis2_temp = server.gui.add_text(
                "lis2_temp",
                initial_value="No Data"
                )
        esp_temp = server.gui.add_text(
                "esp_temp",
                initial_value="No Data"
                )
    zone_angles = compute_zone_angles()
    print("Server at http://localhost:8080")
    while True:
        try:
            process_frame(server, zone_angles, sport, lsm6_temp, lis2_temp, esp_temp)
            time.sleep(0.1)
        except KeyboardInterrupt:
            print("Shutting down");
            break
        finally:
            pass
            #TODO stop serial reader


if __name__ == "__main__":
    main()
