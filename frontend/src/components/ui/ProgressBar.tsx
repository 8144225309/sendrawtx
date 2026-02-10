import type { HTMLAttributes } from 'react';

interface AcceptanceBarProps extends HTMLAttributes<HTMLDivElement> {
  accepted: number;
  total: number;
  size?: 'sm' | 'md' | 'lg';
}

function AcceptanceBar({ accepted, total, size = 'md', className = '', ...props }: AcceptanceBarProps) {
  const percentage = total > 0 ? Math.round((accepted / total) * 100) : 0;

  // Color based on acceptance rate
  const getColor = () => {
    if (percentage >= 80) return 'bg-[#22c55e]';
    if (percentage >= 60) return 'bg-[#84cc16]';
    if (percentage >= 40) return 'bg-[#eab308]';
    if (percentage >= 20) return 'bg-[#f97316]';
    return 'bg-[#ef4444]';
  };

  const sizes = {
    sm: 'h-1.5',
    md: 'h-3',
    lg: 'h-4',
  };

  return (
    <div className={className} {...props}>
      <div className="flex justify-between items-center mb-2">
        <span className="text-sm text-[#a0a0a0]">Endpoint Acceptance</span>
        <span className="text-sm font-medium text-white">
          {percentage}% ({accepted}/{total} endpoints)
        </span>
      </div>
      <div className={`w-full bg-[#252525] rounded-full overflow-hidden ${sizes[size]}`}>
        <div
          className={`${sizes[size]} rounded-full transition-all duration-500 ${getColor()}`}
          style={{ width: `${percentage}%` }}
        />
      </div>
    </div>
  );
}

export { AcceptanceBar };
