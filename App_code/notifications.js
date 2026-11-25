const listeners = new Set();

export const onNotification = (fn) => {
  listeners.add(fn);
  return () => listeners.delete(fn);      // call this to unsubscribe
};

export const emitNotification = (n) => listeners.forEach((fn) => fn(n));