import { forwardRef, type TextareaHTMLAttributes } from 'react';

interface TextareaProps extends TextareaHTMLAttributes<HTMLTextAreaElement> {
  error?: string;
  label?: string;
}

const Textarea = forwardRef<HTMLTextAreaElement, TextareaProps>(
  ({ error, label, className = '', id, ...props }, ref) => {
    const inputId = id || label?.toLowerCase().replace(/\s+/g, '-');

    return (
      <div className="w-full">
        {label && (
          <label
            htmlFor={inputId}
            className="block text-sm font-medium text-[#a0a0a0] mb-1.5"
          >
            {label}
          </label>
        )}
        <textarea
          ref={ref}
          id={inputId}
          className={`
            w-full px-4 py-3 rounded-lg resize-none
            bg-[#252525] border border-[#333]
            text-white placeholder-[#666] font-mono text-sm
            focus:outline-none focus:border-[#f7931a] focus:ring-1 focus:ring-[#f7931a]
            disabled:opacity-50 disabled:cursor-not-allowed
            transition-colors
            ${error ? 'border-[#ef4444] focus:border-[#ef4444] focus:ring-[#ef4444]' : ''}
            ${className}
          `}
          {...props}
        />
        {error && (
          <p className="mt-1.5 text-sm text-[#ef4444]">{error}</p>
        )}
      </div>
    );
  }
);

Textarea.displayName = 'Textarea';

export { Textarea };
