export type LogLevel = "DEBUG" | "INFO" | "WARN" | "ERROR";

export const LOG_LEVELS: LogLevel[] = ["DEBUG", "INFO", "WARN", "ERROR"];

export interface LogEvent {
  ts_ms: number;
  level: LogLevel;
  module: string;
  message: string;
  raw: string;
}

export type LogsMode = "tail" | "mock" | "idle";

export interface LogWireFrame {
  type: "event" | "meta";
  mode?: LogsMode;
  ts_ms?: number;
  level?: LogLevel;
  module?: string;
  message?: string;
  raw?: string;
}
