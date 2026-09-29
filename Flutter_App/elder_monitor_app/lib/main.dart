import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

void main() {
  runApp(const ElderMonitorApp());
}

class ElderMonitorApp extends StatelessWidget {
  const ElderMonitorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Elder Monitor',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.light,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF1565C0),
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: const Color(0xFFF4F7FB),
        cardTheme: CardThemeData(
          elevation: 0,
          color: Colors.white,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(22),
          ),
        ),
        appBarTheme: const AppBarTheme(
          centerTitle: false,
          elevation: 0,
          backgroundColor: Colors.transparent,
          foregroundColor: Color(0xFF102A43),
        ),
      ),
      home: const ElderMonitorHome(),
    );
  }
}

class AlertItem {
  final String time;
  final String title;
  final String message;
  final IconData icon;
  final Color color;

  AlertItem({
    required this.time,
    required this.title,
    required this.message,
    required this.icon,
    required this.color,
  });
}

class _ElderMonitorHomeState extends State<ElderMonitorHome> {
  final Guid serviceUuid = Guid('12345678-1234-1234-1234-1234567890ab');
  final Guid alertCharUuid = Guid('abcd1234-5678-90ab-cdef-1234567890ab');

  BluetoothDevice? connectedDevice;
  BluetoothCharacteristic? alertCharacteristic;

  StreamSubscription<List<ScanResult>>? scanSubscription;
  StreamSubscription<List<int>>? notifySubscription;
  StreamSubscription<BluetoothConnectionState>? connectionStateSubscription;

  bool isScanning = false;
  bool isConnected = false;

  String connectionText = 'Not connected';
  String latestMessage = 'No alert received yet';
  String currentMode = 'NORMAL';

  int? batteryPercent;
  DateTime? lastBatteryUpdateTime;

  final List<AlertItem> alertHistory = [];

  Future<void> requestPermissions() async {
    if (Platform.isAndroid) {
      await Permission.bluetoothScan.request();
      await Permission.bluetoothConnect.request();
      await Permission.locationWhenInUse.request();
    }
  }

  Future<void> connectToBand() async {
    try {
      await scanSubscription?.cancel();
      await notifySubscription?.cancel();
      await connectionStateSubscription?.cancel();

      setState(() {
        isScanning = true;
        connectionText = 'Requesting Bluetooth permission...';
      });

      await requestPermissions();

      final bool supported = await FlutterBluePlus.isSupported;
      if (!supported) {
        setState(() {
          isScanning = false;
          connectionText = 'Bluetooth is not supported on this phone';
        });
        return;
      }

      setState(() {
        connectionText = 'Waiting for phone Bluetooth...';
      });

      await FlutterBluePlus.adapterState
          .where((state) => state == BluetoothAdapterState.on)
          .first;

      setState(() {
        connectionText = 'Scanning for ElderlyBand...';
      });

      BluetoothDevice? foundDevice;

      scanSubscription = FlutterBluePlus.onScanResults.listen((results) {
        for (final ScanResult result in results) {
          final String deviceName = result.device.platformName;
          final String advName = result.advertisementData.advName;

          if (deviceName == 'ElderlyBand' || advName == 'ElderlyBand') {
            foundDevice = result.device;
            break;
          }
        }
      });

      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 8));
      await Future.delayed(const Duration(seconds: 8));
      await FlutterBluePlus.stopScan();
      await scanSubscription?.cancel();

      if (foundDevice == null) {
        setState(() {
          isScanning = false;
          if (!isConnected) {
            connectionText = 'ElderlyBand not found';
          }
        });
        return;
      }

      connectedDevice = foundDevice;

      setState(() {
        connectionText = 'Connecting to ElderlyBand...';
      });

      try {
        await connectedDevice!.connect(
          timeout: const Duration(seconds: 10),
          license: License.nonprofit,
        );
      } catch (_) {
        // Ignore already-connected errors and continue to service discovery.
      }

      connectionStateSubscription = connectedDevice!.connectionState.listen(
        (BluetoothConnectionState state) {
          if (!mounted) return;
          if (state == BluetoothConnectionState.disconnected) {
            setState(() {
              isConnected = false;
              connectionText = 'Disconnected from ElderlyBand';
            });
          }
        },
      );

      setState(() {
        connectionText = 'Finding BLE service...';
      });

      final List<BluetoothService> services =
          await connectedDevice!.discoverServices();

      for (final BluetoothService service in services) {
        if (service.uuid == serviceUuid) {
          for (final BluetoothCharacteristic characteristic
              in service.characteristics) {
            if (characteristic.uuid == alertCharUuid) {
              alertCharacteristic = characteristic;

              notifySubscription = characteristic.onValueReceived.listen(
                (value) {
                  final String message = utf8.decode(value);
                  handleIncomingMessage(message);
                },
              );

              await characteristic.setNotifyValue(true);

              setState(() {
                isConnected = true;
                isScanning = false;
                connectionText = 'Connected to ElderlyBand';
                latestMessage = 'Waiting for alert...';
              });

              addHistory(
                title: 'Device connected',
                message: 'ElderlyBand is ready for monitoring',
                icon: Icons.bluetooth_connected,
                color: Colors.green,
              );

              return;
            }
          }
        }
      }

      setState(() {
        isScanning = false;
        connectionText = 'Alert characteristic not found';
      });
    } catch (e) {
      setState(() {
        isScanning = false;
        connectionText = 'Error: $e';
      });
    }
  }

  void handleIncomingMessage(String message) {
    final String cleanMessage = message.trim();

    // Battery update from ESP32.
    // Accepts both old firmware message BATTERY_EST:100
    // and new firmware message BATTERY:100.
    // The UI displays only the percentage, without showing the word estimated.
    if (cleanMessage.startsWith('BATTERY:') ||
        cleanMessage.startsWith('BATTERY_EST:')) {
      final List<String> parts = cleanMessage.split(':');
      if (parts.length >= 2) {
        final int? percent = int.tryParse(parts[1]);
        if (percent != null) {
          setState(() {
            batteryPercent = percent.clamp(0, 100);
            lastBatteryUpdateTime = DateTime.now();
          });
        }
      }
      return;
    }

    IconData icon = Icons.notifications_active;
    Color color = Colors.blueGrey;
    String title = 'Device message';
    String mode = currentMode;

    if (cleanMessage.startsWith('EMERGENCY:')) {
      mode = 'EMERGENCY';
      title = 'Emergency confirmed';
      icon = Icons.sos;
      color = Colors.red;
    } else if (cleanMessage.contains('PANIC FALL ALERT')) {
      mode = 'FALL WARNING';
      title = 'Fall warning';
      icon = Icons.personal_injury;
      color = Colors.red;
    } else if (cleanMessage.contains('PANIC BUTTON')) {
      mode = 'PANIC WARNING';
      title = 'Panic button alert';
      icon = Icons.warning_rounded;
      color = Colors.red;
    } else if (cleanMessage.contains('INACTIVITY')) {
      mode = 'INACTIVITY WARNING';
      title = 'Inactivity alert';
      icon = Icons.access_time_filled;
      color = Colors.orange;
    } else if (cleanMessage == 'SLEEP_MODE_ON') {
      mode = 'SLEEP';
      title = 'Sleep mode enabled';
      icon = Icons.nightlight_round;
      color = Colors.indigo;
    } else if (cleanMessage == 'SLEEP_MODE_OFF') {
      mode = 'NORMAL';
      title = 'Sleep mode disabled';
      icon = Icons.check_circle;
      color = Colors.green;
    } else if (cleanMessage.startsWith('CANCELLED:')) {
      mode = 'NORMAL';
      title = 'Warning cancelled';
      icon = Icons.cancel;
      color = Colors.green;
    } else if (cleanMessage.startsWith('STOPPED:')) {
      mode = 'NORMAL';
      title = 'Emergency stopped';
      icon = Icons.stop_circle;
      color = Colors.green;
    } else if (cleanMessage == 'DEVICE_STARTED') {
      mode = 'NORMAL';
      title = 'Device started';
      icon = Icons.power_settings_new;
      color = Colors.green;
    }

    setState(() {
      currentMode = mode;
      latestMessage = cleanMessage;
      connectionText = isConnected ? 'Connected to ElderlyBand' : connectionText;
    });

    addHistory(
      title: title,
      message: cleanMessage,
      icon: icon,
      color: color,
    );
  }

  void addHistory({
    required String title,
    required String message,
    required IconData icon,
    required Color color,
  }) {
    final DateTime now = DateTime.now();
    final String time =
        '${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}:${now.second.toString().padLeft(2, '0')}';

    setState(() {
      alertHistory.insert(
        0,
        AlertItem(
          time: time,
          title: title,
          message: message,
          icon: icon,
          color: color,
        ),
      );

      if (alertHistory.length > 50) {
        alertHistory.removeLast();
      }
    });
  }

  Future<void> disconnectBand() async {
    await scanSubscription?.cancel();
    await notifySubscription?.cancel();
    await connectionStateSubscription?.cancel();

    if (connectedDevice != null) {
      await connectedDevice!.disconnect();
    }

    setState(() {
      connectedDevice = null;
      alertCharacteristic = null;
      isConnected = false;
      isScanning = false;
      connectionText = 'Disconnected';
      latestMessage = 'No alert received yet';
      currentMode = 'NORMAL';
    });
  }

  void clearHistory() {
    setState(() {
      alertHistory.clear();
      latestMessage = 'History cleared';
    });
  }

  Color getStatusColor() {
    if (currentMode.contains('EMERGENCY') ||
        currentMode.contains('FALL') ||
        currentMode.contains('PANIC')) {
      return Colors.red;
    }

    if (currentMode.contains('INACTIVITY')) {
      return Colors.orange;
    }

    if (currentMode == 'SLEEP') {
      return Colors.indigo;
    }

    return Colors.green;
  }

  IconData getStatusIcon() {
    if (currentMode.contains('EMERGENCY')) {
      return Icons.sos;
    }

    if (currentMode.contains('FALL')) {
      return Icons.personal_injury;
    }

    if (currentMode.contains('PANIC')) {
      return Icons.warning_rounded;
    }

    if (currentMode.contains('INACTIVITY')) {
      return Icons.access_time_filled;
    }

    if (currentMode == 'SLEEP') {
      return Icons.nightlight_round;
    }

    return Icons.verified_rounded;
  }

  Color getBatteryColor() {
    final int percent = batteryPercent ?? 0;

    if (percent <= 20) {
      return Colors.red;
    }

    if (percent <= 50) {
      return Colors.orange;
    }

    return Colors.green;
  }

  IconData getBatteryIcon() {
    final int percent = batteryPercent ?? 0;

    if (percent <= 15) return Icons.battery_alert;
    if (percent <= 30) return Icons.battery_2_bar;
    if (percent <= 50) return Icons.battery_4_bar;
    if (percent <= 75) return Icons.battery_5_bar;
    return Icons.battery_full;
  }

  String getBatteryUpdateText() {
    if (batteryPercent == null || lastBatteryUpdateTime == null) {
      return 'Waiting for device update';
    }

    final int seconds = DateTime.now().difference(lastBatteryUpdateTime!).inSeconds;

    if (seconds < 5) return 'Updated just now';
    if (seconds < 60) return 'Updated ${seconds}s ago';

    final int minutes = seconds ~/ 60;
    return 'Updated ${minutes}m ago';
  }

  @override
  void dispose() {
    scanSubscription?.cancel();
    notifySubscription?.cancel();
    connectionStateSubscription?.cancel();
    connectedDevice?.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final Color statusColor = getStatusColor();

    return Scaffold(
      appBar: AppBar(
        title: const Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Elder Monitor',
              style: TextStyle(fontWeight: FontWeight.w800),
            ),
            SizedBox(height: 2),
            Text(
              'Smart emergency monitoring band',
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w400),
            ),
          ],
        ),
        actions: [
          Container(
            margin: const EdgeInsets.only(right: 16),
            padding: const EdgeInsets.all(10),
            decoration: BoxDecoration(
              color: isConnected
                  ? Colors.green.withValues(alpha: 0.12)
                  : Colors.grey.withValues(alpha: 0.15),
              borderRadius: BorderRadius.circular(16),
            ),
            child: Icon(
              isConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
              color: isConnected ? Colors.green : Colors.grey,
            ),
          ),
        ],
      ),
      body: SafeArea(
        child: RefreshIndicator(
          onRefresh: () async {
            if (!isConnected && !isScanning) {
              await connectToBand();
            }
          },
          child: ListView(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
            children: [
              _StatusHeroCard(
                statusColor: statusColor,
                icon: getStatusIcon(),
                currentMode: currentMode,
                latestMessage: latestMessage,
                isConnected: isConnected,
              ),
              const SizedBox(height: 14),
              Row(
                children: [
                  Expanded(
                    child: _ConnectionCard(
                      isConnected: isConnected,
                      isScanning: isScanning,
                      connectionText: connectionText,
                      onConnect: connectToBand,
                      onDisconnect: disconnectBand,
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 14),
              _BatteryCard(
                batteryPercent: batteryPercent,
                batteryColor: getBatteryColor(),
                batteryIcon: getBatteryIcon(),
                updateText: getBatteryUpdateText(),
              ),
              const SizedBox(height: 14),
              _QuickInfoGrid(
                connectionStatus: isConnected ? 'Online' : 'Offline',
                mode: currentMode,
                historyCount: alertHistory.length,
              ),
              const SizedBox(height: 22),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Text(
                    'Alert History',
                    style: TextStyle(
                      fontSize: 20,
                      fontWeight: FontWeight.w800,
                      color: Color(0xFF102A43),
                    ),
                  ),
                  TextButton.icon(
                    onPressed: alertHistory.isEmpty ? null : clearHistory,
                    icon: const Icon(Icons.delete_outline),
                    label: const Text('Clear'),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              if (alertHistory.isEmpty)
                const _EmptyHistoryCard()
              else
                ...alertHistory.map((item) => _HistoryTile(item: item)),
            ],
          ),
        ),
      ),
    );
  }
}

class ElderMonitorHome extends StatefulWidget {
  const ElderMonitorHome({super.key});

  @override
  State<ElderMonitorHome> createState() => _ElderMonitorHomeState();
}

class _StatusHeroCard extends StatelessWidget {
  final Color statusColor;
  final IconData icon;
  final String currentMode;
  final String latestMessage;
  final bool isConnected;

  const _StatusHeroCard({
    required this.statusColor,
    required this.icon,
    required this.currentMode,
    required this.latestMessage,
    required this.isConnected,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(22),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [
            statusColor,
            statusColor.withValues(alpha: 0.75),
          ],
        ),
        borderRadius: BorderRadius.circular(28),
        boxShadow: [
          BoxShadow(
            color: statusColor.withValues(alpha: 0.28),
            blurRadius: 24,
            offset: const Offset(0, 12),
          ),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Container(
                padding: const EdgeInsets.all(14),
                decoration: BoxDecoration(
                  color: Colors.white.withValues(alpha: 0.18),
                  borderRadius: BorderRadius.circular(20),
                ),
                child: Icon(icon, color: Colors.white, size: 38),
              ),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                decoration: BoxDecoration(
                  color: Colors.white.withValues(alpha: 0.18),
                  borderRadius: BorderRadius.circular(999),
                ),
                child: Text(
                  isConnected ? 'LIVE' : 'OFFLINE',
                  style: const TextStyle(
                    color: Colors.white,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.7,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 26),
          const Text(
            'Current Status',
            style: TextStyle(
              color: Colors.white70,
              fontSize: 14,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          Text(
            currentMode,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 32,
              fontWeight: FontWeight.w900,
            ),
          ),
          const SizedBox(height: 10),
          Text(
            latestMessage,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 15,
              height: 1.35,
            ),
          ),
        ],
      ),
    );
  }
}

class _ConnectionCard extends StatelessWidget {
  final bool isConnected;
  final bool isScanning;
  final String connectionText;
  final VoidCallback onConnect;
  final VoidCallback onDisconnect;

  const _ConnectionCard({
    required this.isConnected,
    required this.isScanning,
    required this.connectionText,
    required this.onConnect,
    required this.onDisconnect,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: isConnected
                        ? Colors.green.withValues(alpha: 0.12)
                        : Colors.blueGrey.withValues(alpha: 0.12),
                    borderRadius: BorderRadius.circular(16),
                  ),
                  child: Icon(
                    isConnected ? Icons.bluetooth_connected : Icons.bluetooth,
                    color: isConnected ? Colors.green : Colors.blueGrey,
                  ),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text(
                        'Device Connection',
                        style: TextStyle(
                          fontSize: 16,
                          fontWeight: FontWeight.w800,
                          color: Color(0xFF102A43),
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        connectionText,
                        style: TextStyle(color: Colors.grey.shade700),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            Row(
              children: [
                Expanded(
                  child: FilledButton.icon(
                    onPressed: isScanning || isConnected ? null : onConnect,
                    icon: isScanning
                        ? const SizedBox(
                            width: 18,
                            height: 18,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.search),
                    label: Text(isScanning ? 'Scanning...' : 'Connect Band'),
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: isConnected ? onDisconnect : null,
                    icon: const Icon(Icons.link_off),
                    label: const Text('Disconnect'),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _BatteryCard extends StatelessWidget {
  final int? batteryPercent;
  final Color batteryColor;
  final IconData batteryIcon;
  final String updateText;

  const _BatteryCard({
    required this.batteryPercent,
    required this.batteryColor,
    required this.batteryIcon,
    required this.updateText,
  });

  @override
  Widget build(BuildContext context) {
    final double progress = batteryPercent == null ? 0 : batteryPercent! / 100;

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: batteryColor.withValues(alpha: 0.12),
                    borderRadius: BorderRadius.circular(16),
                  ),
                  child: Icon(batteryIcon, color: batteryColor, size: 30),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text(
                        'Device Battery',
                        style: TextStyle(
                          fontSize: 16,
                          fontWeight: FontWeight.w800,
                          color: Color(0xFF102A43),
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        updateText,
                        style: TextStyle(color: Colors.grey.shade700),
                      ),
                    ],
                  ),
                ),
                Text(
                  batteryPercent == null ? '--%' : '$batteryPercent%',
                  style: TextStyle(
                    color: batteryColor,
                    fontSize: 28,
                    fontWeight: FontWeight.w900,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            ClipRRect(
              borderRadius: BorderRadius.circular(999),
              child: LinearProgressIndicator(
                value: progress,
                minHeight: 10,
                backgroundColor: Colors.grey.shade200,
                valueColor: AlwaysStoppedAnimation<Color>(batteryColor),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _QuickInfoGrid extends StatelessWidget {
  final String connectionStatus;
  final String mode;
  final int historyCount;

  const _QuickInfoGrid({
    required this.connectionStatus,
    required this.mode,
    required this.historyCount,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: _MiniInfoCard(
            title: 'Connection',
            value: connectionStatus,
            icon: Icons.sensors,
          ),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: _MiniInfoCard(
            title: 'Mode',
            value: mode,
            icon: Icons.health_and_safety,
          ),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: _MiniInfoCard(
            title: 'Events',
            value: historyCount.toString(),
            icon: Icons.history,
          ),
        ),
      ],
    );
  }
}

class _MiniInfoCard extends StatelessWidget {
  final String title;
  final String value;
  final IconData icon;

  const _MiniInfoCard({
    required this.title,
    required this.value,
    required this.icon,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Icon(icon, color: const Color(0xFF1565C0)),
            const SizedBox(height: 10),
            Text(
              title,
              style: TextStyle(
                fontSize: 12,
                color: Colors.grey.shade600,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(height: 4),
            Text(
              value,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(
                fontSize: 15,
                color: Color(0xFF102A43),
                fontWeight: FontWeight.w800,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _HistoryTile extends StatelessWidget {
  final AlertItem item;

  const _HistoryTile({required this.item});

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.only(bottom: 10),
      child: ListTile(
        contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
        leading: Container(
          padding: const EdgeInsets.all(10),
          decoration: BoxDecoration(
            color: item.color.withValues(alpha: 0.12),
            borderRadius: BorderRadius.circular(14),
          ),
          child: Icon(item.icon, color: item.color),
        ),
        title: Text(
          item.title,
          style: const TextStyle(
            fontWeight: FontWeight.w800,
            color: Color(0xFF102A43),
          ),
        ),
        subtitle: Padding(
          padding: const EdgeInsets.only(top: 3),
          child: Text(item.message),
        ),
        trailing: Text(
          item.time,
          style: TextStyle(
            color: Colors.grey.shade600,
            fontSize: 12,
            fontWeight: FontWeight.w700,
          ),
        ),
      ),
    );
  }
}

class _EmptyHistoryCard extends StatelessWidget {
  const _EmptyHistoryCard();

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 32),
        child: Column(
          children: [
            Icon(Icons.notifications_none, size: 46, color: Colors.grey.shade500),
            const SizedBox(height: 12),
            const Text(
              'No alerts yet',
              style: TextStyle(
                fontSize: 18,
                fontWeight: FontWeight.w800,
                color: Color(0xFF102A43),
              ),
            ),
            const SizedBox(height: 6),
            Text(
              'Device events will appear here after connection.',
              textAlign: TextAlign.center,
              style: TextStyle(color: Colors.grey.shade600),
            ),
          ],
        ),
      ),
    );
  }
}
