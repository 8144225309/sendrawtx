import type { ReactNode, HTMLAttributes } from 'react';

interface BadgeProps extends HTMLAttributes<HTMLSpanElement> {
  variant?: 'default' | 'success' | 'error' | 'warning' | 'info';
  size?: 'sm' | 'md';
  children: ReactNode;
}

function Badge({ variant = 'default', size = 'md', children, className = '', ...props }: BadgeProps) {
  const variants = {
    default: 'bg-[#252525] text-[#a0a0a0]',
    success: 'bg-[#22c55e20] text-[#22c55e]',
    error: 'bg-[#ef444420] text-[#ef4444]',
    warning: 'bg-[#eab30820] text-[#eab308]',
    info: 'bg-[#3b82f620] text-[#3b82f6]',
  };

  const sizes = {
    sm: 'px-2 py-0.5 text-xs',
    md: 'px-2.5 py-1 text-sm',
  };

  return (
    <span
      className={`inline-flex items-center font-medium rounded-md ${variants[variant]} ${sizes[size]} ${className}`}
      {...props}
    >
      {children}
    </span>
  );
}

// Difficulty-specific badge
interface DifficultyBadgeProps extends HTMLAttributes<HTMLSpanElement> {
  difficulty: 'trivial' | 'easy' | 'moderate' | 'hard' | 'extreme' | 'near_impossible';
}

function DifficultyBadge({ difficulty, className = '', ...props }: DifficultyBadgeProps) {
  const colors = {
    trivial: 'bg-[#22c55e20] text-[#22c55e]',
    easy: 'bg-[#84cc1620] text-[#84cc16]',
    moderate: 'bg-[#eab30820] text-[#eab308]',
    hard: 'bg-[#f9731620] text-[#f97316]',
    extreme: 'bg-[#ef444420] text-[#ef4444]',
    near_impossible: 'bg-[#dc262620] text-[#dc2626]',
  };

  const labels = {
    trivial: 'Trivial',
    easy: 'Easy',
    moderate: 'Moderate',
    hard: 'Hard',
    extreme: 'Extreme',
    near_impossible: 'Near Impossible',
  };

  return (
    <span
      className={`inline-flex items-center px-2.5 py-1 text-sm font-medium rounded-md ${colors[difficulty]} ${className}`}
      {...props}
    >
      {labels[difficulty]}
    </span>
  );
}

export { Badge, DifficultyBadge };
