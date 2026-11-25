import { useEffect, useId, useRef } from 'react'

interface ConfirmDialogProps {
  open: boolean
  title: string
  description: string
  confirmLabel: string
  danger?: boolean
  busy?: boolean
  onConfirm: () => void
  onCancel: () => void
}

export function ConfirmDialog({
  open,
  title,
  description,
  confirmLabel,
  danger = false,
  busy = false,
  onConfirm,
  onCancel,
}: ConfirmDialogProps) {
  const dialogRef = useRef<HTMLDialogElement>(null)
  const titleId = useId()

  useEffect(() => {
    const dialog = dialogRef.current
    if (!dialog) return
    if (open && !dialog.open) {
      dialog.showModal()
    } else if (!open && dialog.open) {
      dialog.close()
    }
  }, [open])

  useEffect(() => {
    const dialog = dialogRef.current
    if (!dialog) return
    const handleClose = () => {
      dialog.returnValue = ''
      if (open) onCancel()
    }
    dialog.addEventListener('close', handleClose)
    return () => dialog.removeEventListener('close', handleClose)
  }, [open, onCancel])

  return (
    <dialog
      ref={dialogRef}
      aria-labelledby={titleId}
      className="m-auto w-full max-w-md rounded-xl border border-slate-700 bg-[#111827] p-6 text-slate-100 shadow-2xl"
    >
      <h1 id={titleId} className="text-base font-semibold text-slate-100">
        {title}
      </h1>
      <p className="mt-2 text-sm text-slate-300">{description}</p>
      <div className="mt-6 flex justify-end gap-3">
        <button
          type="button"
          autoFocus={danger}
          disabled={busy}
          onClick={onCancel}
          className="rounded-lg border border-slate-700 bg-slate-800 px-4 py-2 text-sm font-medium text-slate-200 hover:bg-slate-700 disabled:opacity-50"
        >
          Cancel
        </button>
        <button
          type="button"
          disabled={busy}
          onClick={onConfirm}
          className={`rounded-lg px-4 py-2 text-sm font-medium text-white disabled:opacity-50 ${
            danger
              ? 'bg-rose-600 hover:bg-rose-500'
              : 'bg-cyan-600 hover:bg-cyan-500'
          }`}
        >
          {busy ? 'Processing...' : confirmLabel}
        </button>
      </div>
    </dialog>
  )
}
