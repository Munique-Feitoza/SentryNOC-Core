# Protocolo de telemetria

O transporte usa NDJSON: cada linha representa uma amostra independente.

```json
{
  "v": 1,
  "type": "telemetry",
  "seq": 42,
  "ts": 1792947219733,
  "cpu": {
    "user": 635787,
    "nice": 108,
    "system": 69850,
    "idle": 10289113,
    "iowait": 8552,
    "irq": 20359,
    "softirq": 8225,
    "steal": 0
  },
  "memory_kb": {
    "total": 16259964,
    "available": 10445164,
    "swap_total": 9227464,
    "swap_free": 9227464
  },
  "psi": {
    "memory": {
      "some_avg10": 0,
      "full_avg10": 0,
      "some_total_us": 206,
      "full_total_us": 204
    },
    "io": {
      "some_avg10": 0,
      "full_avg10": 0,
      "some_total_us": 9669270,
      "full_total_us": 9002194
    }
  },
  "disk": {
    "read_ops": 210611,
    "read_sectors": 10828220,
    "write_ops": 252820,
    "write_sectors": 28455292,
    "io_ms": 176575
  },
  "network": {
    "rx_bytes": 2315122598,
    "rx_packets": 1867828,
    "rx_errors": 0,
    "rx_drops": 1836,
    "tx_bytes": 372775300,
    "tx_packets": 1127110,
    "tx_errors": 0,
    "tx_drops": 0
  }
}
```

## Campos de controle

- `v`: versão do contrato.
- `type`: tipo do evento; atualmente `telemetry`.
- `seq`: sequência monotônica do processo do agente.
- `ts`: Unix epoch em milissegundos.

## Semântica dos dados

- CPU, disco e rede são contadores cumulativos do kernel.
- `memory_kb` representa valores instantâneos.
- PSI `*_avg10` é a pressão média dos últimos dez segundos, em percentual.
- PSI `*_total_us` é o tempo acumulado de pressão em microssegundos.

## Ingestão

```http
POST /api/v1/ingest
Content-Type: application/x-ndjson
Authorization: Bearer <token>

{"v":1,"type":"telemetry",...}
```

O header `Authorization` é exigido quando `SENTRYNOC_TOKEN` está configurado.
