"use client";

interface SparklineProps {
  values: number[];
  width?: number;
  height?: number;
  color?: string;
  fill?: boolean;
  strokeWidth?: number;
}

export function Sparkline({
  values,
  width = 120,
  height = 28,
  color = "var(--color-cyan-500)",
  fill = true,
  strokeWidth = 1.4,
}: SparklineProps) {
  if (values.length < 2) return <svg viewBox={`0 0 ${width} ${height}`} className="block w-full" style={{ height }} />;

  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min || 1;
  const stepX = width / (values.length - 1);

  const points = values.map((v, i) => {
    const x = i * stepX;
    const y = height - ((v - min) / range) * (height - 2) - 1;
    return `${x.toFixed(2)},${y.toFixed(2)}`;
  });

  const path = `M${points.join(" L")}`;
  const area = fill ? `${path} L${width},${height} L0,${height} Z` : null;
  const gid = `spark-${Math.random().toString(36).slice(2, 9)}`;

  return (
    // viewBox + 100% width = sparkline scales to whatever the parent card
    // happens to be wide. preserveAspectRatio="none" lets the X axis stretch
    // independently of the Y axis (which is what we want for time-series).
    <svg
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="none"
      className="block w-full"
      style={{ height }}
    >
      <defs>
        <linearGradient id={gid} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor={color} stopOpacity="0.35" />
          <stop offset="100%" stopColor={color} stopOpacity="0" />
        </linearGradient>
      </defs>
      {area && <path d={area} fill={`url(#${gid})`} />}
      <path
        d={path}
        fill="none"
        stroke={color}
        strokeWidth={strokeWidth}
        strokeLinejoin="round"
        strokeLinecap="round"
        // Keep the line crisp regardless of how much the X axis was stretched.
        vectorEffect="non-scaling-stroke"
      />
    </svg>
  );
}
