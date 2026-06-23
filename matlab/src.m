%% Sensor Network — Gateway Connection, Logging, and Live Plotting
% Connects to the ESP32 gateway over TCP, parses STATUS/ERROR/DATA messages,
% logs everything to a timestamped text file, and plots live sensor data.

clear; clc; close all;

%% Configuration
GATEWAY_IP   = "192.168.0.180";   % updated each each session
GATEWAY_PORT = 8080;
LOG_FILE     = "sensor_log.txt";

ROOM_NAMES = ["A", "B", "C"];      % must match gateway's ROOMA/ROOMB/ROOMC

%% Connect to Gateway
fprintf("Connecting to gateway at %s:%d...\n", GATEWAY_IP, GATEWAY_PORT);
gateway = tcpclient(GATEWAY_IP, GATEWAY_PORT, "Timeout", 10);
configureTerminator(gateway, "LF");
fprintf("Connected to gateway.\n");

logFile = fopen(LOG_FILE, "a");
if logFile == -1
    error("Could not open log file for writing.");
end

%% Data Storage for Plotting
maxPoints = 200; % rolling window of points shown on the plot

timeData = NaT(maxPoints, 3);              % timestamp per room
tempData = nan(maxPoints, 3);
humData  = nan(maxPoints, 3);
luxData  = nan(maxPoints, 3);

writeIdx = ones(1, 3); % next write index per room (circular buffer)

%% Set Up Live Plot
fig = figure("Name", "Sensor Network — Live Data", "NumberTitle", "off");

subplot(3,1,1); hold on;
tempLines = gobjects(1,3);
for r = 1:3
    tempLines(r) = plot(NaT, NaN, "-x", "DisplayName", "Room " + ROOM_NAMES(r));
end
ylabel("Temperature (°C)"); legend; grid on; title("Temperature");

subplot(3,1,2); hold on;
humLines = gobjects(1,3);
for r = 1:3
    humLines(r) = plot(NaT, NaN, "-x", "DisplayName", "Room " + ROOM_NAMES(r));
end
ylabel("Humidity (%)"); legend; grid on; title("Humidity");

subplot(3,1,3); hold on;
luxLines = gobjects(1,3);
for r = 1:3
    luxLines(r) = plot(NaT, NaN, "-x", "DisplayName", "Room " + ROOM_NAMES(r));
end
ylabel("Light (lux)"); xlabel("Time"); legend; grid on; title("Light Level");

%% Helper: Write to Console + Log File
function logLine(logFile, prefix, msg)
    timestamp = datestr(now, "dd-mmm-yyyy HH:MM:SS");
    line = sprintf("[%s] %s%s", timestamp, prefix, msg);
    if startsWith(prefix, "ERROR")
        fprintf(2, "%s\n", line); % stderr -> red in console
    else
        fprintf("%s\n", line);
    end
    fprintf(logFile, "%s\n", line);
end

%% Main Loop
logLine(logFile, "", "MATLAB connected to gateway");

while true
    if gateway.NumBytesAvailable > 0
        line = readline(gateway);
        line = strtrim(line);
        if isempty(line)
            continue;
        end

        if startsWith(line, "STATUS:")
            msg = extractAfter(line, "STATUS:");
            logLine(logFile, "STATUS: ", msg);

        elseif startsWith(line, "ERROR:")
            msg = extractAfter(line, "ERROR:");
            logLine(logFile, "ERROR: ", msg);

        elseif startsWith(line, "DATA:")
            % Format: DATA:ROOMX:temp,humidity,lux
            body = extractAfter(line, "DATA:");
            parts = split(body, ":");
            if numel(parts) ~= 2
                continue;
            end
            roomToken = parts(1);     % e.g. "ROOMA"
            values = split(parts(2), ",");
            if numel(values) ~= 3
                continue;
            end

            roomLetter = extractAfter(roomToken, "ROOM"); % "A", "B", or "C"
            r = find(ROOM_NAMES == roomLetter, 1);
            if isempty(r)
                continue;
            end

            temp = str2double(values(1));
            hum  = str2double(values(2));
            lux  = str2double(values(3));

            % Shift buffers (simple rolling window)
            idx = writeIdx(r);
            if idx > maxPoints
                timeData(1:end-1, r) = timeData(2:end, r);
                tempData(1:end-1, r) = tempData(2:end, r);
                humData(1:end-1, r)  = humData(2:end, r);
                luxData(1:end-1, r)  = luxData(2:end, r);
                idx = maxPoints;
            end

            timeData(idx, r) = datetime("now");
            tempData(idx, r) = temp;
            humData(idx, r)  = hum;
            luxData(idx, r)  = lux;
            writeIdx(r) = idx + 1;

            % Update plots
            validIdx = ~isnan(tempData(:, r));
            set(tempLines(r), "XData", timeData(validIdx, r), "YData", tempData(validIdx, r));
            set(humLines(r),  "XData", timeData(validIdx, r), "YData", humData(validIdx, r));
            set(luxLines(r),  "XData", timeData(validIdx, r), "YData", luxData(validIdx, r));

            drawnow limitrate;

            fprintf("Room %s -> Temp: %.2f°C  Humidity: %.2f%%  Lux: %.2f\n", ...
                    roomLetter, temp, hum, lux);
        end
    end

    pause(0.05); % avoid hammering the CPU while polling
end

%% Cleanup (only reached if loop is broken manually)
fclose(logFile);
clear gateway;