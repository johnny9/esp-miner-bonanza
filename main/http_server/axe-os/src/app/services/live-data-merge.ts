function isObject(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

/**
 * Apply the recursively generated WebSocket diff without dropping unchanged
 * fields from nested objects such as asicHealth. Arrays remain replacements.
 */
export function mergeLiveDataUpdate<T>(current: T, update: unknown): T {
  if (!isObject(current) || !isObject(update)) {
    return update as T;
  }

  const merged: Record<string, unknown> = { ...current };
  for (const [key, value] of Object.entries(update)) {
    merged[key] = isObject(merged[key]) && isObject(value)
      ? mergeLiveDataUpdate(merged[key], value)
      : value;
  }
  return merged as T;
}
