# מדריך משתמש — FSO Gateway Web GUI

מסמך זה מתאר באופן מפורט כל מסך, כל פאנל וכל פרמטר ב-Web GUI של FSO Gateway. עבור כל שדה מוסבר:

- **המשמעות** — מה הערך מייצג מבחינת רשת/FEC/מערכת.
- **המקור** — מאיפה הערך מגיע: שם הפונקציה ב-C, קובץ ושורה (כשרלוונטי).
- **יחידות** — קצב (Mbps / pps), זמן (ms / שניות), מנייה, יחס, וכו'.
- **כיצד לפרש** — מה נחשב תקין, ערכי סף, מתי כדאי לדאוג.

המסמך מתאר את גרסה `webgui-v1.3.0` (Bridge `1.3.0`, סכמת טלמטריה `fso-gw-stats/2`).

---

## תוכן עניינים

1. [ארכיטקטורה — שלוש השכבות](#ארכיטקטורה--שלוש-השכבות)
2. [Top Bar — סרגל עליון](#top-bar--סרגל-עליון)
3. [Sidebar — סרגל ניווט](#sidebar--סרגל-ניווט)
4. [Dashboard — לוח מחוונים ראשי](#dashboard--לוח-מחוונים-ראשי)
5. [Link Status — מצב הקישור](#link-status--מצב-הקישור)
6. [Traffic — תעבורה](#traffic--תעבורה)
7. [FEC Analytics — אנליטיקת FEC](#fec-analytics--אנליטיקת-fec)
8. [Interleaver — אינטרליבר](#interleaver--אינטרליבר)
9. [Packet Inspector — בוחן חבילות](#packet-inspector--בוחן-חבילות)
10. [Channel — בקרת ערוץ](#channel--בקרת-ערוץ)
11. [Configuration — תצורה](#configuration--תצורה)
12. [System Logs — יומני מערכת](#system-logs--יומני-מערכת)
13. [Alerts — התראות](#alerts--התראות)
14. [Topology — טופולוגיה](#topology--טופולוגיה)
15. [Analytics — אנליטיקה והקלטות](#analytics--אנליטיקה-והקלטות)
16. [About — אודות](#about--אודות)
17. [נספח א' — מילון מונחים](#נספח-א--מילון-מונחים)
18. [נספח ב' — שרשרת המקור של כל מונה](#נספח-ב--שרשרת-המקור-של-כל-מונה)

---

## ארכיטקטורה — שלוש השכבות

ה-GUI אינו מודד דבר בעצמו. כל המספרים מקורם בתוכנת C שרצה כדאמון על המכונה. שרשרת הנתונים:

```
[Linux NIC] → [C Daemon: fso_gw_runner]
                ├─ stats_container        (מונים אטומיים)
                ├─ deinterleaver_t         (סטטיסטיקות RX FEC)
                ├─ arp_cache_t             (טבלת IP↔MAC)
                └─ control_server          (AF_UNIX socket @ 10 Hz)
                          │
                          ▼  /tmp/fso_gw.sock  (ndjson, schema "fso-gw-stats/2")
                  [FastAPI Bridge: webgui/server/main.py + gateway_source.py]
                          ├─ ממיר מבני C → JSON ידידותי לדפדפן
                          ├─ מחשב נגזרות (Mbps, pps, qualityPct, alerts)
                          └─ שומר היסטוריה ב-SQLite (runs/samples)
                          │
                          ▼  WebSocket  /ws/live  (1 Hz snapshots)
                          ▼  REST       /api/*    (config, channel, runs, daemon)
                          ▼  WebSocket  /ws/logs  (יומני stderr)
                  [Next.js GUI: webgui/src/]
                          └─ דפדפן ב-http://<host>:3100
```

**משמעות מעשית:**

- *הדאמון מודד הכל ב-C atomics* — שום ערך לא "מהונדס" ב-GUI. אם המונה הרלוונטי לא קיים ב-C, השדה לא קיים ב-GUI (או מסומן במפורש כ-Mock).
- *ה-Bridge מחשב dt-deltas* — קצבים (Mbps, pps, blk/s) מחושבים ב-Python מהפרשי מונים בין שני snapshots עוקבים.
- *ה-Frontend הוא רק "צייר"* — כל hook (`useTelemetry`, `useChannel`, וכו') מקבל JSON, שומר history קצרה בזיכרון, וגלגל עיגול עם ECharts/Tailwind.
- *Mock vs. Live* — אם הסוקט `/tmp/fso_gw.sock` לא זמין, ה-Bridge עובר ל-`SimState` (סימולציה ב-Python) שמייצר ערכים סינתטיים באותה סכמה. ה-Connection Pill בשמאל למעלה מציין "live" או "demo".

**קבצי מפתח ב-C (להפניות במסמך):**

| תפקיד | קובץ |
|---|---|
| מונים אטומיים גלובליים | [src/stats.c](../../src/stats.c), [include/stats.h](../../include/stats.h) |
| שרת הטלמטריה (UNIX socket) | [src/control_server.c](../../src/control_server.c) |
| FEC RX (deinterleave + decode) | [src/deinterleaver.c](../../src/deinterleaver.c) |
| FEC TX (fragment + encode + interleave) | [src/tx_pipeline.c](../../src/tx_pipeline.c), [src/interleaver.c](../../src/interleaver.c) |
| ARP cache (proxy-ARP) | [src/arp_cache.c](../../src/arp_cache.c) |
| קליטת/שליחת פריימים גולמיים | [src/packet_io.c](../../src/packet_io.c) |
| CRC-32C על-סימבול | [src/symbol.c](../../src/symbol.c) |
| תצורה (CLI args) | [src/config.c](../../src/config.c) |

**קבצי Bridge (Python):**

| תפקיד | קובץ |
|---|---|
| FastAPI app + WebSocket /ws/live | [webgui/server/main.py](../server/main.py) |
| חיבור ל-control_server, נגזרות, התראות | [webgui/server/gateway_source.py](../server/gateway_source.py) |
| בקרת `tc netem` | [webgui/server/channel.py](../server/channel.py) |
| ניהול תצורה (YAML) | [webgui/server/config_store.py](../server/config_store.py) |
| Tail של `/tmp/fso_gw.log` | [webgui/server/log_source.py](../server/log_source.py) |
| Recorder + יצוא CSV | [webgui/server/run_store.py](../server/run_store.py) |
| Daemon supervisor | [webgui/server/daemon.py](../server/daemon.py) |

---

## Top Bar — סרגל עליון

מקור: [webgui/src/components/layout/TopBar.tsx](../src/components/layout/TopBar.tsx)

הסרגל העליון מציג סטטוס בזמן אמת ללא התלות בעמוד. הוא קבוע (`sticky`) בראש המסך.

### כותרת

| שדה | מקור | פירוט |
|---|---|---|
| **FSO Gateway · Control Center** | hardcoded | טקסט קבוע |
| **v3.1 · BUILD a202b70** | hardcoded | placeholder; **לא** משקף את גרסת ה-Bridge בפועל. הגרסה האמיתית מופיעה ב-`/health` ובעמוד About. |

### Connection Pill (חוט החיבור)

| ערך | משמעות |
|---|---|
| **LIVE** ירוק | ה-WebSocket `/ws/live` פתוח וה-Bridge מחזיר נתונים מהסוקט של ה-C daemon. |
| **DEMO** כחול | ה-WebSocket פתוח אבל ה-Bridge לא מצליח לקרוא את `/tmp/fso_gw.sock` — מציג נתוני סימולציה (`SimState` ב-`main.py`). |
| **CONNECTING** ענבר | ה-WebSocket בתהליך התחברות / איבד חיבור ומנסה להתחבר מחדש (backoff). |

המשתנה מנוהל ב-`useTelemetry.ts` לפי `WebSocket.readyState` והודעת `source` שמגיעה ב-payload.

### System

מציג את מצב הקישור הכולל (לא רק התעבורה — אלא איכות ה-FEC).

| תווית | תנאי | משמעות |
|---|---|---|
| **OPERATIONAL** ירוק | `qualityPct > 99.5%` | המערכת תקינה. כמעט כל הבלוקים משוחזרים בהצלחה. |
| **DEGRADED** ענבר | `95% < qualityPct ≤ 99.5%` | יש איבודי בלוקים מורגשים — בודקים את ערוץ ה-FSO/השמיים. |
| **OFFLINE** אדום | `qualityPct ≤ 95%` | מצב חמור — לא מצליחים לשחזר. ייתכן ניתוק פיזי. |

**חישוב:** `qualityPct = 100 × blocks_recovered / blocks_attempted`. אם `blocks_attempted == 0` (טרם זרמו בלוקים), הערך הוא `100%` כברירת מחדל.

מקור הנתונים:
- `blocks_attempted`: [src/stats.c](../../src/stats.c) — `stats_inc_block_attempt()`. נקרא ב-[src/deinterleaver.c](../../src/deinterleaver.c) כשהבלוק עובר ל-`FILLING`.
- `blocks_recovered`: `stats_inc_block_success()` — נקרא בסיום פענוח מוצלח של בלוק.
- חישוב הסף נמצא ב-[gateway_source.py](../server/gateway_source.py) פונקציית `snapshot()`.

### Daemon (חדש ב-1.3.0)

מציג את מצב הדאמון לפי ה-DaemonSupervisor שב-Bridge.

| תווית | משמעות |
|---|---|
| **STOPPED** אפור | אין תהליך פעיל. אפשר ללחוץ Start בעמוד Configuration. |
| **STARTING** ענבר נושם | הופעל subprocess; ה-Bridge ממתין לראות שלא מת מיד. |
| **RUNNING** ירוק | התהליך חי. `pid != null`. |
| **STOPPING** ענבר נושם | נשלח SIGTERM, ממתינים עד 4 שניות לפני SIGKILL. |
| **FAILED** אדום | התהליך מת באופן לא צפוי, או הקובץ הבינארי לא נמצא. ה-`lastError` יוצג בעמוד Configuration. |

**הערה חשובה:** המצב משקף את ה-supervisor ב-Bridge — לא את ה-C daemon עצמו. אם הדאמון קרס בלי שה-supervisor שם לב (תיאורטי), ה-watcher task יעדכן את המצב ל-`failed` תוך כדי `proc.wait()`.

מקור: [webgui/server/daemon.py](../server/daemon.py) — `DaemonSupervisor.status()`. ה-hook הוא [useDaemon](../src/lib/useDaemon.ts) שעושה poll ל-`/api/daemon` כל 2 שניות.

### Uptime

זמן שעבר מאז שהדאמון פתח את שרת הטלמטריה.

- **מקור:** `link.uptimeSec` שמגיע ישירות מ-`uptime_sec` ב-snapshot של ה-control_server. החישוב הוא `CLOCK_MONOTONIC` בתוך [src/control_server.c](../../src/control_server.c) — `elapsed_seconds()`.
- **פורמט:** `formatUptime()` ב-[utils.ts](../src/lib/utils.ts) — `HhMMm` או `MMm SSs`.
- **לא** ה-uptime של המכונה / Bridge — רק של תהליך הדאמון.

### Time (UTC)

שעון פשוט מהדפדפן (`new Date().toISOString().slice(11, 19)`) שמתעדכן כל שנייה. לא קשור לדאמון.

### אייקונים בקצה הימני (Search / Maximize / Bell)

כפתורים קוסמטיים. ה-Bell מציג נקודה אדומה תמיד — placeholder, **לא** מחובר עדיין לזרם ההתראות.

---

## Sidebar — סרגל ניווט

מקור: [webgui/src/components/layout/Sidebar.tsx](../src/components/layout/Sidebar.tsx)

סרגל קבוע משמאל עם 13 פריטי ניווט. כל פריט הוא Link של Next.js.

| פריט | נתיב | תיאור קצר |
|---|---|---|
| Dashboard | `/` | מבט-על: סטטוס, throughput, שגיאות. |
| Link Status | `/link-status` | היסטוריית איכות + אירועי דעיכה (fades). |
| Traffic | `/traffic` | תעבורה מפורטת: TX/RX, peak/avg, גודל חבילה ממוצע. |
| FEC Analytics | `/fec-analytics` | תוצאות בלוקים, היסטוגרמת bursts, dil_stats. |
| Interleaver | `/interleaver` | ויזואליזציית מטריצת ה-K×M, חישוב burst recoverable. |
| Packet Inspector | `/packet-inspector` | פורמט הסימבול על החוט (header), אירועי CRC. |
| Channel | `/channel` | בקרת tc netem — הזרקת איבודי חבילות לבדיקת FEC. |
| Configuration | `/configuration` | פרמטרי FEC + Runtime Controls. |
| System Logs | `/logs` | זרם logs חי מהדאמון. |
| Alerts | `/alerts` | היסטוריית התראות + סינון. |
| Topology | `/topology` | מפת המכונות ה-FSO + ARP cache. |
| Analytics | `/analytics` | רשימת runs מוקלטים + יצוא CSV. |
| About | `/about` | מצב המערכת ומידע גרסאות. |

### פס הבריאות בתחתית

`Health 98.7%` — **placeholder** (hardcoded). אינו מחובר ל-API בשום צורה. מיועד לחיווט עתידי.

---

## Dashboard — לוח מחוונים ראשי

מקור: [webgui/src/app/page.tsx](../src/app/page.tsx)

מורכב מ-5 פאנלים שמתעדכנים כל שנייה דרך WebSocket `/ws/live`.

### LinkStatusHero — דיאגרמת ה-Gateway

מקור: [webgui/src/components/dashboard/LinkStatusHero.tsx](../src/components/dashboard/LinkStatusHero.tsx)

פאנל עם דיאגרמה: `Win-1 ── GW-A ══ GW-B ── Win-2`.

| שדה | מקור | פירוט |
|---|---|---|
| **שם LAN/FSO iface** | `configEcho.lanIface`, `configEcho.fsoIface` | משתקף ישירות מ-CLI args שעם הם רץ הדאמון, דרך [src/control_server.c](../../src/control_server.c) (`configEcho`). |
| **תוויות "Win-1 / Win-2"** | hardcoded | טקסט קבוע לפיקסטור Phase 8 (192.168.50.1, 192.168.50.2). |
| **Status Dot ירוק/אדום** | נגזר מ-`link.state` | ירוק אם `state != "offline"`. |
| **STATE (טקסט גדול)** | `link.state` | "ONLINE" / "DEGRADED" / "OFFLINE". |
| **Quality %** | `link.qualityPct` | הצלחה כוללת של פענוח בלוקים. |
| **K=N · M=N · depth=N** | `configEcho.k`, `.m`, `.depth` | קונפיגורציה פעילה של הדאמון. |

### ThroughputCards — כרטיסי תעבורה

מקור: [webgui/src/components/dashboard/ThroughputCards.tsx](../src/components/dashboard/ThroughputCards.tsx)

ארבעה כרטיסים: TX, RX, Packets/sec, Utilization.

#### TX Throughput

- **ערך גדול** — `throughput[-1].txBps` בפורמט Mbps/Gbps.
- **Sparkline** — 60 הדגימות האחרונות של `txBps`.
- **תת-טקסט** — `txPps` (חבילות בשנייה).

**כיצד מחושב ב-Bridge:**
```
txBps = (transmitted_bytes_now - transmitted_bytes_prev) × 8 / dt_seconds
txPps = (transmitted_packets_now - transmitted_packets_prev) / dt_seconds
```

המקור ב-C:
- `transmitted_packets`, `transmitted_bytes` ב-[src/stats.c](../../src/stats.c) — `stats_inc_transmitted(size_t bytes)`.
- נקרא ב-[src/tx_pipeline.c](../../src/tx_pipeline.c) שורה ~554 כשסימבול נשלח לחוט (FSO TX).

> **שים לב:** `transmitted` נספר *לפי סימבול* ולא לפי חבילה אורגינלית. כל חבילת LAN שנכנסת מתפרקת ל-K סימבולים + M כפילויות, וכל אחד מהם נספר. זה הקצב על *חוט ה-FSO*.

#### RX Throughput

- **ערך גדול** — `throughput[-1].rxBps`.
- **תת-טקסט** — `rxPps`.

**מחושב באותו אופן** מ-`recovered_bytes` / `recovered_packets`.

המקור:
- `stats_inc_recovered(size_t bytes)` ב-[src/stats.c](../../src/stats.c).
- נקרא ב-[src/rx_pipeline.c](../../src/rx_pipeline.c) (~שורה 463, 502, 529) כשחבילה הוחזרה בהצלחה לאחר reassembly + LAN TX.

> **שים לב:** `recovered` הוא *חבילה שלמה ומשוחזרת*, לא סימבול. אם בלוק נכשל לפענח, אף חבילה ממנו לא נספרת. כך ש-`rxBps` מודד תעבורה *לכודת לקוח* בצד השני, לא רק נכנסת לדאמון.

#### Packets/sec

- **ערך גדול** — `txPps`.
- **תת-טקסט** — `rxPps`.

#### Link Utilization

- **ערך גדול** — `(latestTxBps + latestRxBps) / (10 Gbps × 2) × 100`.
- **חישוב לקוח-צד בלבד**, מניח קישור 10Gbps לכל כיוון.
- **טון**: ירוק <80%, ענבר ≥80%.
- **לא** מבוסס על קצב פיזי בפועל של ה-NIC — רק חישוב יחסי שרירותי.

### ErrorMetrics — מטריקות שגיאה

מקור: [webgui/src/components/dashboard/ErrorMetrics.tsx](../src/components/dashboard/ErrorMetrics.tsx)

חמישה כרטיסים על פני הרוחב.

#### Symbol Loss Ratio

- **משמעות:** אחוז הסימבולים שאבדו בערוץ ה-FSO ולא הגיעו ל-deinterleaver.
- **חישוב:** `lost_symbols / total_symbols`.
- **מקור ב-C:** `stats_record_symbol(bool lost)` ב-[src/stats.c](../../src/stats.c) שורה ~441-476. נקרא ב-[src/rx_pipeline.c](../../src/rx_pipeline.c) פעם אחת לכל סימבול שהדאמון מנסה לפרש.
- **תצוגה:** נוטציה מדעית כשהיחס < 1e-4 (`5.2e-5`), אחוזים אחרת.
- **ערכי סף:**
  - `< 0.01%` — ערוץ נקי.
  - `0.01–1%` — לחות/חלקיקים. עדיין בתוך תקציב FEC.
  - `> 5%` — הולכים להיות failures.

#### Block Fail Rate

- **משמעות:** אחוז הבלוקים שלא הצליחו להיפענח כלל.
- **חישוב:** `blocks_failed / blocks_attempted`.
- **מקור:** `stats_inc_block_failure()` נקרא מ-[src/deinterleaver.c](../../src/deinterleaver.c) במקרים: timeout (לא הגיעו מספיק סימבולים בזמן), too-many-holes (יותר מ-M חורים).
- **טון:** ירוק 0%, ענבר >0.1%, אדום >0.1%.

#### CRC Drops

- **משמעות:** מספר הסימבולים שנפסלו בבדיקת ה-CRC-32C על-סימבול (אם הופעל).
- **מקור:** `stats_inc_crc_drop_symbol()` ב-[src/stats.c](../../src/stats.c) שורה ~485. נקרא ב-[src/rx_pipeline.c](../../src/rx_pipeline.c) או [src/symbol.c](../../src/symbol.c) כשה-CRC לא תואם.
- **שימוש:** סימבול עם CRC-fail מטופל כ-erasure לא ידוע (FEC יכולה להחליף אותו אם יש מספיק גיבוי).
- **מתי לדאוג:** אם המספר עולה במהירות, ה-link מקבל corruption (לא רק איבוד).

#### Recovered Packets

- **משמעות:** מונה מצטבר של חבילות LAN שהשוחזרו והועברו הלאה.
- **מקור:** `recovered_packets` ב-[src/stats.c](../../src/stats.c), `stats_inc_recovered()`.
- **קריאה:** ב-[src/rx_pipeline.c](../../src/rx_pipeline.c) שורות 463, 502, 529 (שלושה מצבי reassembly מוצלח).

#### FEC Success Rate

- **חישוב:** `(blocksRecovered / blocksAttempted) × 100`.
- **מחושב לקוח-צד** (מאותם שדות שמגיעים ב-`errors`).
- **טון:** ירוק >99.9%, ציאן >99%, ענבר אחרת.

### LiveCharts — גרפי זמן-אמת

מקור: [webgui/src/components/dashboard/LiveCharts.tsx](../src/components/dashboard/LiveCharts.tsx)

#### Throughput Over Time

- שתי סדרות: TX Mbps (ציאן) ו-RX Mbps (כחול מקווקו).
- 180 דגימות אחרונות (3 דקות ב-1Hz).
- ציר Y ב-Mbps; ציר X זמן יחסי.

#### Packet Rate (pps)

- שתי סדרות: TX pps, RX pps.
- 180 דגימות אחרונות.

#### Burst-Length Distribution

- היסטוגרמה של 7 בינים: `1`, `2-5`, `6-10`, `11-50`, `51-100`, `101-500`, `501+`.
- **מקור ב-C:** `burst_len_1` ... `burst_len_501_plus` ב-[src/stats.c](../../src/stats.c) שורות 50-56.
- **כיצד מחושב burst:** `stats_record_symbol()` סופר רצף של סימבולים אבודים רצופים. כשיש סימבול שלא אבד, ה-burst נסגר ומסווג לבין המתאים ב-`stats_close_current_burst()` ([src/stats.c](../../src/stats.c) שורות 99-115).
- **צבעים:** ציאן/כחול לבינים שניתנים לתיקון (במסגרת `M × depth`), ענבר/אדום לחורגים.

#### Cumulative Counters

- **Blocks with loss** — `decoderStress.blocksWithLoss` ← `blocks_with_loss` ב-[src/stats.c](../../src/stats.c) שורה 66. בלוקים שאחרי deinterleave היו עם לפחות חור אחד.
- **Total holes** — `decoderStress.totalHolesInBlocks` ← סכום כל החורים על פני כל הבלוקים.
- **Recoverable bursts** — `recoverable_bursts` ב-[src/stats.c](../../src/stats.c) שורה 60. burst שאורכו ≤ `M × depth`.
- **Critical bursts** — `critical_bursts` ב-[src/stats.c](../../src/stats.c) שורה 61. burst שחרג ולא יכול היה להישחזר.

---

## Link Status — מצב הקישור

מקור: [webgui/src/app/link-status/page.tsx](../src/app/link-status/page.tsx)

### Hero Tiles — אריחים ראשיים

| שדה | מקור | פירוט |
|---|---|---|
| **STATE** | `link.state` | "ONLINE" / "DEGRADED" / "OFFLINE" — נגזר ב-Bridge. |
| **Quality %** | `link.qualityPct` | `(blocks_recovered / blocks_attempted) × 100`. |
| **Symbol Loss** | `errors.symbolLossRatio` | יחס; מוצג כאחוזים או 1e-X. |
| **Block Fail Rate** | `errors.blockFailRatio` | `blocks_failed / blocks_attempted`. |
| **Blocks Attempted** | `errors.blocksAttempted` | מצטבר מאז הפעלת הדאמון. |
| **Session Uptime** | `link.uptimeSec` | מאותו מקור כמו ב-TopBar. |

### Quality Over Time

גרף קו של `qualityPct` לאורך זמן. ציר Y בטווח 80–100% להגדלת הרגישות.

- **מקור:** [useLinkHistory](../src/lib/useLinkHistory.ts) שואב כל snapshot ושומר היסטוריה לקוח-צד (לא נשמר בשרת).
- **חלון:** עד מאות דגימות; משתמש בזיכרון הדפדפן בלבד.

### Symbol Loss Ratio

גרף לוגריתמי של יחס איבודי הסימבולים לאורך זמן.

- **ציר Y לוגריתמי** כדי לתפוס טווח רחב (1e-6 עד 1.0).
- מקור הנתונים זהה לדגימת ה-Quality.

### Stability Panel

| שדה | מקור | הערה |
|---|---|---|
| **Session Uptime %** | חישוב לקוח | יחס זמן ב-`online`/`degraded` מתוך כל הזמן הנצפה. |
| **Observed For** | חישוב לקוח | משך הזמן שהעמוד פתוח. |
| **In Current State** | חישוב לקוח | זמן מאז המעבר האחרון בין מצבים. |
| **Fade Events** | חישוב לקוח | מספר מעברים `online↔degraded↔offline` שזוהו. |

⚠️ **כל הפאנל הזה הוא ניתוח לקוח-צד** של ה-history של ה-WebSocket. אין בצד ה-C מנגנון של "fade detection" — מדובר באנליטיקה שנעשית בדפדפן.

### Fade Timeline

טבלה עם רשומה לכל אירוע דעיכה. כל שורה כוללת:

- **Timestamp** — `tStart` ב-ms.
- **From → To** — מעבר מצבים (`online → degraded`).
- **Lowest Quality** — הערך המינימלי של `qualityPct` במהלך הדעיכה.
- **Duration** — `tEnd - tStart`, או "ongoing".

⚠️ סינתטי באותו האופן.

---

## Traffic — תעבורה

מקור: [webgui/src/app/traffic/page.tsx](../src/app/traffic/page.tsx)

עמוד מפורט יותר על תעבורה — מסתמך על אותם שדות שב-Dashboard.

### TX Card / RX Card

לכל כרטיס:

| שדה | מקור | פירוט |
|---|---|---|
| **Current Rate** | `throughput[-1].txBps` / `.rxBps` | קצב נוכחי ב-Mbps/Gbps. |
| **Current PPS** | `throughput[-1].txPps` / `.rxPps` | חבילות בשנייה. |
| **Utilization** | `(latestBps / 10e9) × 100` | יחסית ל-10Gbps (הנחה שרירותית). |
| **Peak Rate** | `max(history.txBps)` | מקסימום על פני כל ה-history הזמין. |
| **Avg Rate** | `avg(history.txBps)` | ממוצע. |
| **Peak PPS** | `max(history.txPps)` | מקסימום קצב חבילות. |
| **Avg Packet Size** | `latestBps / 8 / latestPps` | חישוב בייטים-לחבילה. |
| **Sparkline** | `throughput.slice(-60)` | 60 דגימות (1 דקה). |

ה-history נשמר בלקוח (`useTelemetry` שומר עד 300 דגימות). אין endpoint נפרד ל-Traffic — הכל מגיע ב-snapshot היחיד.

### Charts

- **Throughput** — שתי סדרות `txBps`/`rxBps` על פני 300 דגימות.
- **PPS** — שתי סדרות `txPps`/`rxPps`.

### Summary Metrics

| שדה | חישוב |
|---|---|
| **Combined Throughput** | `latestTxBps + latestRxBps`. |
| **Combined PPS** | `latestTxPps + latestRxPps`. |
| **Avg TX Packet** | `txBps / 8 / txPps`. |
| **Peak Utilization** | `max(peakTxBps, peakRxBps) / 10e9 × 100`. |

---

## FEC Analytics — אנליטיקת FEC

מקור: [webgui/src/app/fec-analytics/page.tsx](../src/app/fec-analytics/page.tsx)

זה העמוד החשוב ביותר להבנת התנהגות ה-FEC.

### Hero Metrics

| שדה | מקור | פירוט |
|---|---|---|
| **Recovery Rate** | `(blocksRecovered / blocksAttempted) × 100` | זהה ל-Quality באחרים, מוצג ב-FEC context. |
| **Blocks/sec** | ממוצע 5 דגימות אחרונות | קצב פענוח בלוקים. תת-טקסט: recovered/sec ו-failed/sec. |
| **Failed Blocks** | `errors.blocksFailed` | מצטבר. טון לפי אחוז כשל (>0.1% = warning). |
| **Symbol Loss** | `errors.symbolLossRatio` | זהה לעמודים אחרים. |
| **Critical Bursts** | סכום הבינים `101-500` ו-`501+` של `burstHistogram` | מקרים שה-FEC לא יכול היה לכסות. |

### FEC Config Panel

מציג את התצורה הפעילה של הדאמון.

| שדה | מקור |
|---|---|
| **K (Source Symbols)** | `configEcho.k` — מאיפה? CLI flag `--k` של הדאמון, נמסר חזרה ב-snapshot. |
| **M (Repair Symbols)** | `configEcho.m` — `--m`. |
| **Overhead** | `m / (k + m) × 100` — חישוב לקוח. |
| **Code Rate** | `k / (k + m)` — חישוב לקוח. |
| **LAN Interface** | `configEcho.lanIface` — `--lan-iface`. |
| **FSO Interface** | `configEcho.fsoIface` — `--fso-iface`. |
| **Symbol Size** | `configEcho.symbolSize` — `--symbol-size`. |
| **Symbol CRC** | `configEcho.internalSymbolCrc` — "CRC-32C" אם True, "Disabled" אחרת. |

> **למה חשוב להציג את ה-config בכל עמוד?** כי הפרמטרים האלה משפיעים על *משמעות* כל המספרים. burst של 50 קריטי כש-`M × depth = 32`, אבל לא כש-`M × depth = 100`.

### Block Outcomes Over Time

גרף עם שתי סדרות מוערמות (stacked):
- **Recovered blocks/sec** — דלתא של `blocks_recovered` בכל שנייה.
- **Failed blocks/sec** — דלתא של `blocks_failed`.

מקור: [useFecHistory](../src/lib/useFecHistory.ts) שואב מ-snapshot ושומר היסטוריה.

### Burst-Length Distribution

זהה ל-Dashboard, רק עם ציר Y בלוגריתמי (טווח רחב יותר).

### Deinterleaver Panel (חשוב!)

זה המקום היחיד ב-GUI שמציג את ה-`dil_stats` הפנימיים של ה-deinterleaver — מאז גרסה `fso-gw-stats/2`.

| שדה | מקור ב-C | משמעות |
|---|---|---|
| **Active** | `dil_stats.active_blocks` ב-[src/deinterleaver.c](../../src/deinterleaver.c) | בלוקים במצב `FILLING` או `READY_TO_DECODE` — תפוסה נוכחית. |
| **Ready** | `dil_stats.ready_count` | בלוקים שהושלמו ומחכים ל-`deinterleaver_get_ready_block()`. |
| **Blocks ready (cumulative)** | `dil_stats.blocks_ready` | מצטבר מאז ההפעלה. |
| **Failed · timeout** | `dil_stats.blocks_failed_timeout` | בלוקים שפסלו כי לא הגיעו מספיק סימבולים בזמן (timeout = 50ms ברירת מחדל). |
| **Failed · holes** | `dil_stats.blocks_failed_holes` | בלוקים שלא ניתן לפענח כי יש יותר מ-M חורים. |
| **Dropped · duplicate** | `dil_stats.dropped_symbols_duplicate` | סימבול נכפל (אותו fec_id הגיע פעמיים). |
| **Dropped · frozen** | `dil_stats.dropped_symbols_frozen` | סימבול הגיע לבלוק שכבר עזב את `FILLING` (איחר). |
| **Dropped · erasure** | `dil_stats.dropped_symbols_erasure` | סימבול עם `is_erasure=1` (מסומן כאבוד). |
| **Dropped · CRC fail** | `dil_stats.dropped_symbols_crc_fail` | סימבול נפסל ב-CRC-32C. |
| **Evicted · filling** | `dil_stats.evicted_filling_blocks` | סלוט פונה כשעוד היה ב-FILLING (חוסר מקום במאגר). |
| **Evicted · done** | `dil_stats.evicted_done_blocks` | סלוט פונה ב-`READY_TO_DECODE` בלי שהבלוק נדרין. |

> **מי גורם ל-eviction?** ה-deinterleaver ממומש כ-circular buffer. כשמגיעים יותר בלוקים חדשים מהמכסה (`depth × 4`), בלוקים ישנים מפוצצים. זה מצב שלא אמור לקרות בייצור — אם זה קורה, יש backpressure ב-RX.

### Block Lifecycle Event Feed

טבלה של אירועי בלוק אחרונים. מציגה כל מעבר ב-FSM של ה-deinterleaver.

| עמודה | מקור |
|---|---|
| **Timestamp** | `blockEvents[].t` — `now_ms()` ב-[src/control_server.c](../../src/control_server.c). |
| **Reason** | `blockEvents[].reason` — enum ממופה מ-`deinterleaver_block_final_reason_t`. ערכים אפשריים: `SUCCESS`, `DECODE_FAILED`, `TIMEOUT`, `TOO_MANY_HOLES`, `EVICTED_FILLING`, `EVICTED_READY`. |
| **Block ID** | `blockEvents[].blockId` — מהבלוק עצמו. |
| **Evicted** | `blockEvents[].evicted` — bool, אם הבלוק נסגר על ידי eviction. |

מאחורי הקלעים: ה-deinterleaver רושם callback ב-control_server, וכל מעבר סופי דוחף לרשימה (`block_events` ring buffer של ~128 אירועים). ה-Bridge מוציא את זה ב-snapshot.

### Decoder Stress Panel

| שדה | מקור |
|---|---|
| **Blocks with Loss** | `blocks_with_loss` — נספר כשבלוק הסתיים והיה לו לפחות חור אחד. |
| **Worst Holes / Block** | `worst_holes_in_block` — מקסימום חורים בבלוק יחיד. |
| **Total Holes** | `total_holes_in_blocks` — סכום חורים על פני כל הבלוקים. |
| **Recoverable Bursts** | `recoverable_bursts` — burst אורך ≤ `M × depth`. |
| **Critical Bursts** | `critical_bursts` — burst שחרג. |
| **Exceeding FEC Span** | `bursts_exceeding_fec_span` — לרוב זהה ל-Critical (שונה במקרי קצה). |

מקור כללי: [src/stats.c](../../src/stats.c) שורות 58-68. החישוב נעשה ב-`stats_close_current_burst()` (שורות 137-164) שהיא שמסווגת.

---

## Interleaver — אינטרליבר

מקור: [webgui/src/app/interleaver/page.tsx](../src/app/interleaver/page.tsx)

עמוד תיעודי בעיקר — מציג את מבנה המטריצה.

### Metric Cards

| שדה | חישוב | פירוט |
|---|---|---|
| **Matrix (depth × K+M)** | `configEcho.depth × (k + m)` | מימדי המטריצה. |
| **Cell count** | `depth × (k + m)` | סך תאי המטריצה. |
| **Matrix Size** | `cell_count × symbolSize` | גודל בייטים. |
| **Recovery Span** | `m × depth` | המספר המקסימלי של סימבולים רצופים שניתן לאבד ולשחזר. |
| **Burst Coverage** | `(burstWithinSpan / totalBursts) × 100` | כמה אחוז מה-bursts שנצפו היו ניתנים לתיקון. |
| **Exceeding Span** | `decoderStress.burstsExceedingFecSpan` | mid-test failures. |

### Matrix Layout (visual)

ויזואל אינטראקטיבי של המטריצה (`depth` שורות × `K+M` עמודות):
- ה-K עמודות הראשונות צבועות ציאן (data).
- ה-M עמודות האחרונות צבועות ענבר (parity).

### Derivations Panel

חזרה על אותם פרמטרים כתצוגת רשימה:

| שדה | חישוב |
|---|---|
| **Depth (rows)** | `configEcho.depth` |
| **Block width** | `k + m` |
| **Total symbols / block** | `k + m` |
| **Max burst recoverable** | `m × depth` |
| **Symbol size** | `configEcho.symbolSize` |
| **Block size** | `(k + m) × symbolSize` |

---

## Packet Inspector — בוחן חבילות

מקור: [webgui/src/app/packet-inspector/page.tsx](../src/app/packet-inspector/page.tsx)

דף הסבר פורמט הסימבול על החוט + מטריקות פעילות.

### Activity Cards

| שדה | מקור | פירוט |
|---|---|---|
| **Packets/sec (TX)** | `throughput[-1].txPps` | כמו ב-Traffic. |
| **RX packets/sec** | `throughput[-1].rxPps` | תת-טקסט. |
| **Avg Packet Size** | `latestBps / 8 / latestPps` | בייטים. |
| **Symbols each (approx)** | `avgPacketSize / symbolSize` | כמה סימבולים נדרשים לחבילה. |
| **Symbols Processed** | `blocksAttempted × k` | אומדן סך הסימבולים שעיבדנו. |
| **CRC Drops** | `errors.crcDrops` | סך כשלי CRC. |
| **CRC enabled/disabled** | `configEcho.internalSymbolCrc` | טקסט מסביר. |

### Wire Format Diagram

מציג את מבנה ה-symbol header — 18 בייטים קבועים:

| שדה | בייטים | מקור ב-C |
|---|---|---|
| `packet_id` | 4 | [include/symbol.h](../../include/symbol.h) |
| `fec_id` | 4 | המיקום של הסימבול ב-block (0..K-1 = source, K..K+M-1 = parity). |
| `symbol_index` | 2 | מיקום בתוך ה-block אורגינלי. |
| `total_symbols` | 2 | `K+M`. |
| `payload_len` | 2 | אורך data. |
| `crc32` | 4 | CRC-32C על ה-payload (אם הופעל). |
| **payload** | symbolSize | data בפועל (בד"כ 800-1500). |

### Recent Symbol Events

טבלת logs מסוננת לאירועי סימבול (מילות מפתח: `pkt_id`, `block_id`, `symbols`, `fragment`, `reassemble`).

המקור: WebSocket `/ws/logs` שמתחבר ל-[useLogs](../src/lib/useLogs.ts).

⚠️ אם הדאמון לא רץ, ה-Bridge מייצר logs סינתטיים — לא אמיתי.

---

## Channel — בקרת ערוץ

מקור: [webgui/src/app/channel/page.tsx](../src/app/channel/page.tsx) ו-[useChannel.ts](../src/lib/useChannel.ts)

עמוד שמשתמש ב-`tc netem` של לינוקס כדי להזריק איבודי חבילות לערוץ ה-FSO ולבדוק את ה-FEC.

### Presets

ששה presets שמטענים ערכי slider פוטנציאליים. הם **לא** מופעלים אוטומטית — צריך ללחוץ Apply.

| Preset | enterPct | exitPct | lossPct | תיאור |
|---|---|---|---|---|
| **Clear** | 0 | 0 | 0 | כיבוי `netem`, ערוץ נקי. |
| **Drizzle** | 1 | 70 | 0.5 | אובדן נדיר של סימבול בודד. |
| **Weather** | 5 | 50 | 5 | דעיכות בינוניות, עדיין בתוך תקציב FEC. |
| **Haze** | 8 | 30 | 8 | אובדן מתמשך נמוך. |
| **Storm** | 12 | 25 | 20 | bursts כבדים, דוחק את ה-FEC. |
| **Blackout** | 30 | 10 | 50 | אובדן פתולוגי, צפוי שלא ישוחזר. |

### Sliders

| Slider | משמעות | יחידות |
|---|---|---|
| **Enter % (Burst Entry)** | הסתברות מעבר ממצב "good" ל-"bad" של Gilbert-Elliott. | אחוזים 0-100. |
| **Exit % (Burst Exit)** | הסתברות יציאה ממצב "bad". | אחוזים 0-100. |
| **Loss % (in bad state)** | קצב אובדן חבילות במצב "bad". | אחוזים 0-100. |

### Buttons

- **Apply** — שולח POST ל-`/api/channel/netem` עם הערכים. ה-Bridge מריץ:
  ```
  tc qdisc replace dev <FSO_iface> root netem loss gemodel <enter>% <exit>% <loss>%
  ```
  המקור: [webgui/server/channel.py](../server/channel.py) — `apply_gemodel()`.

- **Clear** — שולח POST עם `{clear: true}`. ה-Bridge מריץ `tc qdisc del`.

### Status Display

| שדה | מקור | פירוט |
|---|---|---|
| **iface** | local state, ברירת מחדל `enp1s0f1np1` | אפשר לערוך. |
| **available** | תוצאת `tc qdisc show` | האם ה-binary של `tc` נגיש (בודק על ה-iface). |
| **active** | האם יש netem qdisc פעיל. | |
| **model** | `"gemodel"` / `"uniform"` / `null` | מודל איבוד פעיל. |
| **lossPct / enterPct / exitPct** | parsing של פלט `tc` | ערכים נוכחיים מהמערכת. |

### דרישות הרשאה

`tc qdisc add/del` דורש `CAP_NET_ADMIN`. או ש-Bridge רץ כ-root, או שמוגדר sudoers שירשה `tc` ספציפי. אם לא, השדה `available=false` ויוצג warning.

---

## Configuration — תצורה

מקור: [webgui/src/app/configuration/page.tsx](../src/app/configuration/page.tsx)

עמוד עריכת הקונפיגורציה של הדאמון. שינויים נשמרים ל-`webgui/server/config.yaml` ונדרש *restart* כדי שייכנסו לתוקף.

### FEC Parameters Panel

| פרמטר | טווח | משמעות | חישוב נגזרים |
|---|---|---|---|
| **K — Source Symbols** | 4–256 | כמה סימבולים מקוריים בכל בלוק. K גבוה = פחות overhead, יותר השהיה. | — |
| **M — Repair Symbols** | 1–256 | כמה סימבולים מיותרים (parity). M גבוה = יכולת תיקון טובה יותר. | — |
| **Overhead** | מחושב | `m / (k + m) × 100` — אחוז עודפות. | M=4, K=8 → 33% overhead. |
| **Code Rate** | מחושב | `k / (k + m)` — יעילות מידע. | M=4, K=8 → 0.667. |
| **Block Size** | מחושב | `(k + m) × symbolSize` בייטים. | K=8, M=4, sym=1500 → 18000 bytes. |
| **Burst Recovery ~** | מחושב | `m × depth` — burst מקסימלי שניתן לשחזר. | M=4, depth=16 → 64 symbols. |

### Interleaver Panel

| פרמטר | טווח | משמעות |
|---|---|---|
| **Depth (rows)** | 1–256 | מספר השורות במטריצת האינטרליבר. depth גבוה = פיזור burst יותר טוב, אבל יותר השהיה ויותר זיכרון. |

### Symbol Panel

| פרמטר | טווח | משמעות |
|---|---|---|
| **Symbol Size** | 64–4096 | בייטים של payload בכל סימבול. גדול = פחות overhead, אבל פגיעות גבוהה יותר לאיבוד יחיד. |

### Network Interfaces Panel

| פרמטר | משמעות |
|---|---|
| **LAN Interface** | שם ממשק לתעבורת הלקוחות. הדאמון יבצע `pcap_open` ויקלוט פריימים. |
| **FSO Interface** | שם ממשק לקישור ה-FSO. דרכו נשלחים סימבולים מקודדים. |

### Symbol Integrity Panel

| פרמטר | משמעות |
|---|---|
| **Internal Symbol CRC-32C** | אם פעיל, כל סימבול נושא CRC-32C על ה-payload. ב-RX, אם ה-CRC לא תואם, הסימבול נפסל לפני שמגיע ל-FEC. השפעה: יותר עמידות לקורפציה תוך-ערוץ; פחות throughput (4 bytes/symbol). |

### Action Bar

| כפתור | פעולה |
|---|---|
| **Apply Changes** | שולח POST `/api/config` עם ה-draft. ה-Bridge ולידציה ב-`config_store.validate()`, שמירה ל-YAML. נדרש restart לדאמון בפועל. |
| **Revert** | מבטל שינויים בדראפט הלקוח. |

### Live Derived

חזרה על הנגזרים בצד ימין — רק לתצוגה.

### Runtime Controls (Phase 3B)

מקור: [useDaemon](../src/lib/useDaemon.ts) ו-[webgui/server/daemon.py](../server/daemon.py)

| כפתור | פעולה |
|---|---|
| **Start Gateway** | POST `/api/daemon/start` — ה-supervisor מבצע `subprocess.Popen` עם `argv` שנבנה מ-config.yaml. |
| **Restart** | POST `/api/daemon/restart` — Stop ואז Start. |
| **Stop** | POST `/api/daemon/stop` — שולח SIGTERM, ואחרי 4s SIGKILL. |

תצוגת מצב מפורטת:

| שדה | פירוט |
|---|---|
| **PID** | מזהה תהליך של ה-subprocess. |
| **Uptime** | זמן מאז ה-start. |
| **Sudo** | האם ה-supervisor מקדים `sudo -n` (לפי `FSO_DAEMON_SUDO=1`). |
| **Binary** | "found"/"missing" לפי `os.access(path, X_OK)`. |
| **binary path** | הנתיב לקובץ הבינארי (`FSO_DAEMON_BINARY` או ברירת מחדל `fso_gw_runner`). |
| **log** | קובץ הלוג (`FSO_DAEMON_LOG`, ברירת מחדל `/tmp/fso_gw.log`). |

### Daemon State Badge

ראה [TopBar > Daemon](#daemon-חדש-ב-130) — אותה לוגיקה.

### משתני סביבה רלוונטיים

| משתנה | ברירת מחדל | משמעות |
|---|---|---|
| `FSO_DAEMON_BINARY` | `fso_gw_runner` | נתיב לבינארי שיופעל. אפשר להצביע ל-`control_server_demo` לפיתוח. |
| `FSO_DAEMON_SUDO` | `0` | אם `1`, מוסיף `sudo -n` לפני argv (נדרש בייצור עבור pcap raw). |
| `FSO_DAEMON_LOG` | `/tmp/fso_gw.log` | stdout+stderr מתועדים שם. |
| `FSO_DAEMON_KILL_ON_EXIT` | `0` | אם `1`, ה-Bridge הורג את הדאמון בכיבוי (אחרת — נשאר כ-orphan). |

---

## System Logs — יומני מערכת

מקור: [webgui/src/app/logs/page.tsx](../src/app/logs/page.tsx) ו-[useLogs.ts](../src/lib/useLogs.ts)

זרם logs חי מהדאמון.

### Log Console

| שדה | מקור |
|---|---|
| **Live stream** | WebSocket `/ws/logs`. |
| **Timestamp** | `event.ts_ms` — מ-stderr של הדאמון, או מ-`time.time()` אם generated. |
| **Level** | `event.level` — `DEBUG`/`INFO`/`WARN`/`ERROR` לפי [src/logging.c](../../src/logging.c). |
| **Module** | `event.module` — תת-מערכת (e.g., `tx_pipeline`, `deinterleaver`). |
| **Message** | `event.message` — טקסט. |

### בקרות

| בקרה | פירוט |
|---|---|
| **Level filter** | סינון לפי רמת חומרה. |
| **Module filter** | סינון לתת-מערכת. |
| **Search** | substring search בכל הטקסט. |
| **Pause / Resume** | עוצר את ה-rendering (השרת ממשיך לדחוף). |
| **Export** | ⚠️ עדיין לא מומש. |

### מקור הנתונים

ה-Bridge tails את `/tmp/fso_gw.log` (או `FSO_LOG_FILE`) ב-[log_source.py](../server/log_source.py). אם אין קובץ, מייצר logs סינתטיים ("Mock log entry from sim_runner.c") — מסומן ב-`source: "mock"` בכל אירוע.

### LOG levels ב-C

מקור: [include/logging.h](../../include/logging.h):
```c
typedef enum {
    LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR
} log_level_t;
```

הסינון מתבצע ב-`log_set_level()`. ברירת מחדל בייצור: `INFO`.

---

## Alerts — התראות

מקור: [webgui/src/app/alerts/page.tsx](../src/app/alerts/page.tsx) ו-[useAlerts.ts](../src/lib/useAlerts.ts)

### Alert tabs

- **Active** — התראות שהמשתמש לא סימן כ-acknowledged.
- **Acknowledged** — התראות שסומנו (נשמר ב-`localStorage`).
- **All** — הכל.

### Filtering

- **Severity checkboxes** — `critical` / `warning` / `info`.
- **Module dropdown** — `LINK` / `FEC` / `CRC` / `BURST` / `ARP` / `CONFIG` / `*`.
- **Search** — substring.

### Alert Event Details

| שדה | מקור |
|---|---|
| **ID** | `alert.id` — `{ts}-{module}-{counter}`, נוצר ב-Bridge. |
| **Timestamp** | `alert.t` — `int(time.time() * 1000)` ב-[gateway_source.py](../server/gateway_source.py). |
| **Severity** | `alert.severity` — אחד מ: `critical`/`warning`/`info`. |
| **Module** | `alert.module` — תת-מערכת. |
| **Message** | `alert.message` — טקסט אנושי. |
| **Acknowledged** | `localStorage` — לקוח-צד בלבד. |

### חוקי הפקת התראות (Live)

מקור: [gateway_source.py](../server/gateway_source.py) — `_detect_alerts()`. נקרא בכל snapshot שמגיע, משווה ל-snapshot הקודם.

| תנאי | חומרה | מודול | טקסט |
|---|---|---|---|
| `delta(blocks_failed) >= 3` | **critical** | FEC | "{N} FEC blocks failed this tick" |
| `delta(symbols_dropped_crc) >= 20` | **warning** | CRC | "{N} symbols dropped on CRC" |
| `max_burst_length` עלה ועבר 20 | **warning** | BURST | "Max burst length climbed to {N}" |
| `delta(bursts_exceeding_fec_span) > 0` | **critical** | BURST | "{N} burst(s) exceeded FEC span this tick" |

### Mock alerts (כשהדאמון לא רץ)

ב-[main.py](../server/main.py) — `_maybe_emit_alert()` מייצר התראות בהסתברות ~15% לכל tick, מטמפלטים שרירותיים. מיועד לדמו בלבד.

### Ring buffer

הרשימה ב-Bridge מוגבלת ל-`ALERT_RING_MAX` (כ-200 אירועים). ה-GUI מקבל רק את האחרונות בכל snapshot.

---

## Topology — טופולוגיה

מקור: [webgui/src/app/topology/page.tsx](../src/app/topology/page.tsx)

### Physical Layout

דיאגרמה קבועה (Phase 8 fixture):

```
Win-1 (192.168.50.1) ──[LAN]── GW-A ══[FSO cable]══ GW-B ──[LAN]── Win-2 (192.168.50.2)
```

### Static Labels

- **GW-A / GW-B** — קבועים.
- **Win-1 · 192.168.50.1**, **Win-2 · 192.168.50.2** — קבועים.
- **LAN/FSO iface names** — מ-`configEcho.lanIface`/`.fsoIface`.

### FsoBeam Visualization

קומפוננטה אנימטיבית (Framer Motion) שמדמה קרן אור.

| תכונה | מקור | חישוב |
|---|---|---|
| **Beam color** | `link.state` | ירוק/ענבר/אדום. |
| **Quality % label** | `link.qualityPct` | מוצג בתוך הקרן. |
| **TX intensity** | `min(1, txPps / 5000)` | צפיפות חלקיקים (אנימציה). |
| **RX intensity** | `min(1, rxPps / 5000)` | אותו. |
| **TX/RX pps labels** | `throughput[-1].txPps`/`.rxPps` | "TX 1234 pps". |

### ARP Cache Panel

טבלה עם רשומות שלמדה ה-arp_cache של הדאמון.

| עמודה | מקור |
|---|---|
| **IP** | `arpEntries[].ip` — מ-[src/arp_cache.c](../../src/arp_cache.c). |
| **MAC** | `arpEntries[].mac`. |
| **Last Seen** | `now - arpEntries[].lastSeenMs` — חישוב לקוח, מציג "5s ago". |

מי כותב לטבלה?
- ב-`fso_gw_runner` — `arp_cache_learn()` נקרא ב-[src/rx_pipeline.c](../../src/rx_pipeline.c) כשמזהים `src_ip` של ARP request שמגיע מ-LAN.
- ב-`control_server_demo` — `seed_arp_cache()` קוראת `arp_cache_learn` עם ערכים קבועים של Phase 8 כל 5 שניות (בגלל TTL של 5 דקות).

ה-snapshot כולל את הטבלה דרך `arp_cache_get_entries()` שמחזיר `struct arp_entry[]`.

⚠️ ב-mock mode (Bridge בלי דאמון), הטבלה היא רשימה קבועה של 2 ערכים בלבד.

### Recent Link Events

טבלה זהה לזו שב-Alerts — אבל מסוננת לאירועים האחרונים בלבד.

---

## Analytics — אנליטיקה והקלטות

מקור: [webgui/src/app/analytics/page.tsx](../src/app/analytics/page.tsx) ו-[useRuns.ts](../src/lib/useRuns.ts)

עמוד הקלטה והשוואה של runs היסטוריים. הנתונים נשמרים ב-SQLite (`webgui/server/runs.db`).

### Runs Tab

#### Active Run

| שדה | מקור |
|---|---|
| **activeRunId** | `useRuns().activeRunId` ← `/health` או `/api/runs`. |
| **New Run** | POST `/api/runs/new` → `run_store.create_run()`. יוצר רשומה חדשה ומתחיל לדגום ב-1 Hz. |

#### Runs List

טבלה של כל ה-runs:

| עמודה | מקור |
|---|---|
| **ID** | `run.id` — auto-increment ב-SQLite. |
| **Name** | `run.name` — אופציונלי, ניתן לעריכה. |
| **Start time** | `run.start_ms` — `int(time.time() * 1000)` בעת יצירה. |
| **End time** | `run.end_ms` — null אם פעיל. |
| **Duration** | `(end_ms - start_ms) / 1000`. |
| **Sample count** | `run.sample_count`. |

#### Run Detail

ניתן לבחור run ולראות:

| תרשים | מקור |
|---|---|
| **Throughput over time** | `samples[].txBps`/`rxBps` — נדגם 1 Hz. |
| **Quality (FEC) over time** | `samples[].qualityPct`. |
| **FEC block outcome rate** | `recovered`/`failed` per second. |

נדגם ע"י `_recorder_loop()` ב-[main.py](../server/main.py) — לולאה שכל שנייה לוקחת snapshot ושומרת ל-SQLite.

#### Summary Stats

| שדה | חישוב |
|---|---|
| **Peak/Avg throughput** | `max`/`avg` של `txBps`/`rxBps` על פני כל הדגימות. |
| **Min/Max/Avg quality** | סטטיסטיקות על `qualityPct`. |
| **Total/Recovered/Failed blocks** | מהצילום האחרון של ה-run. |

#### Export CSV

GET `/api/runs/{runId}/export.csv` — מחזיר CSV עם עמודות `t,txBps,rxBps,txPps,rxPps,qualityPct,...`. נוצר ב-[run_store.py](../server/run_store.py) — `export_csv()`.

### Experiments Tab

עמוד placeholder. ה-API קיים (`/api/experiments` ב-[experiments.py](../server/experiments.py)) — קורא קבצי `.txt` מ-`/tmp/fso_experiments/` וממיר. אבל ה-UI עוד לא מציג אותם בצורה מפורטת.

---

## About — אודות

מקור: [webgui/src/app/about/page.tsx](../src/app/about/page.tsx)

עמוד מטה-מידע על המערכת.

### System Status Cards

| כרטיס | שדה | מקור | פירוט |
|---|---|---|---|
| **Bridge** | "Connected" / "Unreachable" | `useHealth()` — GET `/health`. | האם FastAPI מגיב. |
| | "Tick {Hz}" | `health.tick_hz` | קצב הדגימה של ה-Bridge (ברירת מחדל 1Hz). |
| **Gateway** | "Live (control_server)" / "Mock data" | `health.source` | "gateway" אם הסוקט קיים, "mock" אחרת. |
| | "Streaming via UNIX socket" | `health.source == "gateway"` | אינדיקציה. |
| **Recording** | "Run #{id}" / "Not recording" | `health.active_run_id` | מצב ה-recorder. |
| | logs mode | `health.logs_mode` | "live"/"tail"/"file"/"idle". |

### Architecture Diagram

ויזואל קבוע (לא דינמי) של ה-3 השכבות:
```
Browser  ─[WS/REST :8000]─→  FastAPI Bridge  ─[UNIX socket]─→  C Daemon
```

### Tech Stack

רשימה קבועה של ספריות וגרסאות.

### Roadmap

טבלה של פאזות עם סטטוס:

| Phase | Status |
|---|---|
| Phase 1 — Shell + design | Done |
| Phase 2 — FastAPI bridge + control_server | Done |
| Phase 3A — Config persistence | Done |
| Phase 3B — Daemon supervision | Done (v1.3.0) |
| Phase 4 — Feature pages | Done |
| Phase 5 — Analytics + CSV | Done |
| Polish — Cosmetic pass | Pending |

---

## נספח א' — מילון מונחים

| מונח | הגדרה |
|---|---|
| **FEC (Forward Error Correction)** | תיקון שגיאות קדימה. שולחים יותר נתונים ממה שצריך כדי שהצד השני יוכל לתקן איבודים בלי לבקש שידור חוזר. |
| **Wirehair** | ספריית fountain code (codeforce ספציפי) — מקבלת K סימבולי מקור ומייצרת K+M סימבולים. ה-decoder יכול לשחזר אם קיבל לפחות K מתוכם. |
| **Block** | יחידה לוגית של K סימבולי source + M סימבולי parity = K+M סימבולים. |
| **Symbol** | יחידה אטומית של נתון על החוט. כל סימבול נושא header של 18 בייטים + payload (`symbolSize`). |
| **Burst** | רצף של סימבולים אבודים רצופים. ה-FEC עומד מול burst עד `M × depth`. |
| **Interleaver** | מטריצה (`depth` שורות × `K+M` עמודות) שמערבבת סימבולים בזמן השליחה. כך שסימבולים מאותו block לא מופיעים רצוף על החוט — burst של איבוד פיזי לא הורס block יחיד אלא מתפזר על פני כמה. |
| **Deinterleaver** | הצד השני — אוסף סימבולים מהחוט ומחזיר אותם ל-blocks המקוריים. אחראי על דעיכת סימבולים שאיחרו. |
| **CRC-32C (Castagnoli)** | פולינום `0x82F63B78`. בדיקה של 4 בייטים על ה-payload של כל סימבול. אם הופעל, סימבול עם CRC לא תקין נמחק כ-erasure. |
| **AF_UNIX socket** | סוקט תקשורת מקומי (file-based). שרת הטלמטריה של הדאמון פותח אחד בנתיב `/tmp/fso_gw.sock`. |
| **WebSocket** | פרוטוקול תקשורת דו-כיווני מעל HTTP. ה-GUI מתחבר לסוקט `/ws/live` של ה-Bridge ומקבל snapshots. |
| **netem** | Network emulator של לינוקס. רכיב qdisc שיכול להוסיף latency, loss, reorder, וכו'. ה-GUI משתמש בו דרך `tc` להזרקת איבודים. |
| **Gilbert-Elliott model** | מודל אובדן עם שני מצבים ("good"/"bad") עם הסתברויות מעבר ביניהם. נותן bursts ריאליסטיים יותר ממודל uniform. |
| **proxy-ARP** | טכניקה שבה הדאמון עונה ל-ARP requests עבור IPs שמעבר ל-link, וכך מגשר LAN לקוחות מבלי שיצטרכו להגדיר routing. |

---

## נספח ב' — שרשרת המקור של כל מונה

עזר מהיר: כל ערך שנראה ב-GUI, מאיפה הוא בא ב-C.

### TX side

| GUI שדה | C counter | קובץ:שורה | פונקציה |
|---|---|---|---|
| TX throughput (Mbps/pps) | `transmitted_packets`, `transmitted_bytes` | [src/stats.c](../../src/stats.c) | `stats_inc_transmitted()` |
| | | [src/tx_pipeline.c:554](../../src/tx_pipeline.c) | קריאה ב-FSO TX |
| ingress (לא מוצג ישירות, חלק מ-stats) | `ingress_packets`, `ingress_bytes` | [src/stats.c](../../src/stats.c) | `stats_inc_ingress()` |
| | | [src/tx_pipeline.c:311](../../src/tx_pipeline.c) | קריאה כשחבילה נכנסת מ-LAN |

### RX side

| GUI שדה | C counter | קובץ:שורה | פונקציה |
|---|---|---|---|
| RX throughput | `recovered_packets`, `recovered_bytes` | [src/stats.c](../../src/stats.c) | `stats_inc_recovered()` |
| Recovered Packets | זה | [src/rx_pipeline.c:463,502,529](../../src/rx_pipeline.c) | אחרי reassembly מוצלח |
| Failed Packets | `failed_packets` | [src/stats.c](../../src/stats.c) | `stats_inc_failed_packet()` |

### Symbols

| GUI שדה | C counter | קובץ:שורה |
|---|---|---|
| Symbol Loss Ratio | `lost_symbols / total_symbols` | [src/stats.c:441-476](../../src/stats.c) — `stats_record_symbol(bool lost)` |
| CRC Drops | `symbols_dropped_crc` | [src/stats.c:485](../../src/stats.c) — `stats_inc_crc_drop_symbol()` |

### Blocks (FEC)

| GUI שדה | C counter | קובץ:שורה |
|---|---|---|
| Blocks Attempted | `blocks_attempted` | [src/stats.c:470-474](../../src/stats.c) — `stats_inc_block_attempt()` |
| Blocks Recovered | `blocks_recovered` | [src/stats.c:475-479](../../src/stats.c) — `stats_inc_block_success()` |
| Blocks Failed | `blocks_failed` | [src/stats.c:480-484](../../src/stats.c) — `stats_inc_block_failure()` |
| Quality % | computed: `blocks_recovered / blocks_attempted × 100` | [gateway_source.py snapshot()](../server/gateway_source.py) |

### Bursts

| GUI שדה | C counter | קובץ:שורה |
|---|---|---|
| Burst histogram (7 bins) | `burst_len_1`, `_2_5`, `_6_10`, `_11_50`, `_51_100`, `_101_500`, `_501_plus` | [src/stats.c:50-56](../../src/stats.c) |
| Recoverable Bursts | `recoverable_bursts` | [src/stats.c:60](../../src/stats.c), שורה 162 |
| Critical Bursts | `critical_bursts` | [src/stats.c:61](../../src/stats.c), שורה 157 |
| Exceeding FEC Span | `bursts_exceeding_fec_span` | [src/stats.c:58](../../src/stats.c), שורה 158 |
| Max Burst Length | `max_burst_length` | [src/stats.c](../../src/stats.c) — מתעדכן ב-`stats_close_current_burst()` |
| Total Bursts | `total_bursts` | [src/stats.c](../../src/stats.c) |

### Decoder Stress

| GUI שדה | C counter | קובץ:שורה |
|---|---|---|
| Blocks with Loss | `blocks_with_loss` | [src/stats.c:66](../../src/stats.c) — `stats_record_block(holes)` |
| Total Holes | `total_holes_in_blocks` | [src/stats.c:68](../../src/stats.c) |
| Worst Holes / Block | `worst_holes_in_block` | [src/stats.c:68](../../src/stats.c) — מקסימום מתעדכן atomically |

### Deinterleaver Stats (`dil_stats`)

מקור: [src/deinterleaver.c](../../src/deinterleaver.c), מבנה ב-[include/deinterleaver.h](../../include/deinterleaver.h).

| GUI שדה | C field | מתי מתעדכן |
|---|---|---|
| Active | `active_blocks` | בלוקים ב-FILLING + READY_TO_DECODE. |
| Ready | `ready_count` | בלוקים ב-READY_TO_DECODE בלבד. |
| Blocks ready (cumulative) | `blocks_ready` | מצטבר. |
| Failed timeout | `blocks_failed_timeout` | בלוק עזב כי לא הגיעו מספיק symbols ב-50ms. |
| Failed holes | `blocks_failed_holes` | בלוק עזב כי >M חורים. |
| Dropped duplicate | `dropped_symbols_duplicate` | אותו `fec_id` הגיע פעמיים. |
| Dropped frozen | `dropped_symbols_frozen` | symbol הגיע ל-block שלא ב-FILLING. |
| Dropped erasure | `dropped_symbols_erasure` | symbol עם `is_erasure=1`. |
| Dropped CRC fail | `dropped_symbols_crc_fail` | CRC נכשל. |
| Evicted filling | `evicted_filling_blocks` | סלוט פוצץ ב-FILLING. |
| Evicted done | `evicted_done_blocks` | סלוט פוצץ ב-READY_TO_DECODE. |

### ARP Cache

מקור: [src/arp_cache.c](../../src/arp_cache.c), מבנה ב-[include/arp_cache.h](../../include/arp_cache.h).

| GUI שדה | C field |
|---|---|
| ip / mac | `struct arp_entry { uint32_t ip; uint8_t mac[6]; uint64_t last_seen_ms; }` |

### Block Events (FEC Lifecycle)

מקור: callbacks מ-[src/deinterleaver.c](../../src/deinterleaver.c) → ring buffer ב-[src/control_server.c](../../src/control_server.c).

| GUI reason | C enum |
|---|---|
| SUCCESS | `DIL_BLOCK_FINAL_REASON_SUCCESS` |
| DECODE_FAILED | `DIL_BLOCK_FINAL_REASON_DECODE_FAILED` |
| TIMEOUT | `DIL_BLOCK_FINAL_REASON_TIMEOUT` |
| TOO_MANY_HOLES | `DIL_BLOCK_FINAL_REASON_TOO_MANY_HOLES` |
| EVICTED_FILLING | `DIL_BLOCK_FINAL_REASON_EVICTED_FILLING` |
| EVICTED_READY | `DIL_BLOCK_FINAL_REASON_EVICTED_READY` |

### Config Echo

| GUI שדה | C source | CLI flag |
|---|---|---|
| `configEcho.k` | `cfg.k` | `--k` |
| `configEcho.m` | `cfg.m` | `--m` |
| `configEcho.depth` | `cfg.depth` | `--depth` |
| `configEcho.symbolSize` | `cfg.symbol_size` | `--symbol-size` |
| `configEcho.lanIface` | `cfg.lan_iface` | `--lan-iface` |
| `configEcho.fsoIface` | `cfg.fso_iface` | `--fso-iface` |
| `configEcho.internalSymbolCrc` | `cfg.internal_symbol_crc_enabled` | `--internal-symbol-crc 1` |

ה-config מועבר ל-`control_server` ב-`gateway_create()` ([src/gateway.c](../../src/gateway.c)) ונדחף לכל snapshot ב-[src/control_server.c](../../src/control_server.c) פונקציית `build_snapshot_json()`.

### שדות מחושבים (לא ב-C)

| GUI שדה | חישוב | היכן |
|---|---|---|
| Quality % | `100 × blocks_recovered / blocks_attempted` | [gateway_source.py snapshot()](../server/gateway_source.py) |
| txBps / rxBps | `(bytes_delta × 8) / dt_seconds` | gateway_source.py — `_ingest()` |
| txPps / rxPps | `packets_delta / dt_seconds` | אותו מקום |
| Symbol Loss Ratio | `lost_symbols / total_symbols` | gateway_source.py snapshot() |
| Block Fail Rate | `blocks_failed / blocks_attempted` | אותו מקום |
| FEC Success Rate | `(blocks_recovered / blocks_attempted) × 100` | client-side ב-[ErrorMetrics.tsx](../src/components/dashboard/ErrorMetrics.tsx) |
| Link Utilization | `(txBps + rxBps) / 20Gbps × 100` | client-side, קבוע 10Gbps לכל כיוון |
| Burst Coverage | `burstWithinSpan / totalBursts × 100` | client-side ב-Interleaver page |
| Avg Packet Size | `txBps / 8 / txPps` | client-side |
| Symbols each (approx) | `avgPacketSize / symbolSize` | client-side |

### שדות Mock / Fabricated

⚠️ הבאים *לא* מבוססים על C counters — או מחושבים בדפדפן או מסומלצים:

| שדה | היכן | מה המקור |
|---|---|---|
| Daemon state | TopBar, Configuration | `DaemonSupervisor` ב-Bridge — משקף intent של ה-supervisor, לא ה-C process. |
| Fade events (Link Status) | Link Status page | חישוב לקוח על history של quality. |
| Sidebar Health 98.7% | Sidebar | hardcoded. |
| Notification badge | TopBar | hardcoded. |
| Mock alerts | Alerts page (כש-daemon down) | random ב-`main.py:_maybe_emit_alert()`. |
| Mock logs | Logs page (כש-daemon down) | generated ב-[log_source.py](../server/log_source.py). |
| Mock ARP entries | Topology (כש-daemon down) | רשימה קבועה ב-`main.py`. |

---

## תקלות נפוצות

### "Connection: DEMO" למרות שהדאמון רץ

- בדוק שהסוקט קיים: `ls -la /tmp/fso_gw.sock`.
- בדוק שה-Bridge מתחבר: `tail -f /tmp/bridge.log` ותחפש "gateway connected".
- בדוק שאין יותר מתהליך אחד שמנסה להאזין על אותו נתיב.

### "Daemon: FAILED" עם "binary not found"

- בדוק `FSO_DAEMON_BINARY` ב-env של ה-Bridge.
- ודא שהקובץ קיים ושיש לו `+x`.
- ב-Linux ייתכן שצריך `setcap cap_net_raw,cap_net_admin=eip` על הבינארי — או להפעיל עם `sudo`.

### "Daemon: FAILED" עם "exited immediately rc=N"

- הריץ ידנית את אותו argv שמופיע ב-`status.command` כדי לראות את שגיאת ה-stderr.
- ב-`fso_gw_runner` סיבה נפוצה: ה-iface לא קיים או חסרות הרשאות לבצע `pcap_open`.

### יוצאים errors בעמוד Channel ("tc: command not found" / "permission denied")

- ודא ש-`tc` זמין ב-PATH של ה-Bridge.
- לערוץ ה-FSO: או ש-Bridge רץ כ-root, או שהוסיפו ל-sudoers `NOPASSWD: /sbin/tc`.

### גרפי Throughput מציגים אפס

- ודא שיש תעבורה אמיתית מ-LAN. הריץ `iperf3` מ-Win-1 ל-Win-2 כדי לאמת.
- ב-demo mode (`control_server_demo`) — קצב ה-throughput הוא סינתטי ולא תלוי בתעבורה אמיתית.

---

**Document version:** 1.0  
**מתאים לגרסת GUI:** `webgui-v1.3.0`  
**Schema:** `fso-gw-stats/2`  
**עודכן לאחרונה:** 2026-04-25
