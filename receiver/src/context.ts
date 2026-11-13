import { NAMESPACE } from './protocol';

export interface IncomingEvent {
  data: unknown; // a JSON string or an already parsed object
  senderId: string;
}

/** The three calls of a Cast receiver context that this page uses. */
export interface ReceiverContext {
  addCustomMessageListener(namespace: string, listener: (event: IncomingEvent) => void): void;
  sendCustomMessage(namespace: string, senderId: string, data: unknown): void;
  start(): void;
}

/**
 * The context of a real Cast device (the Cast Application Framework). It keeps the connection open, and accepts
 * JSON on the custom namespace (the only type that namespace takes).
 */
export function createCastContext(): ReceiverContext {
  const context = cast.framework.CastReceiverContext.getInstance();
  const options = new cast.framework.CastReceiverOptions();
  options.disableIdleTimeout = true;
  options.customNamespaces = Object.assign({}, options.customNamespaces, {
    [NAMESPACE]: cast.framework.system.MessageType.JSON,
  });
  return {
    addCustomMessageListener: (namespace, listener) =>
      context.addCustomMessageListener(namespace, (event) => listener({ data: event.data, senderId: event.senderId })),
    sendCustomMessage: (namespace, senderId, data) => context.sendCustomMessage(namespace, senderId, data),
    start: () => context.start(options),
  };
}

export interface DevContextHooks {
  /** The WebSocket to the PC receiver is open. */
  onOpen(): void;
  /** The PC receiver says the Cast device was stopped: start fresh. */
  onReset(): void;
}

/**
 * A stand-in for the Cast context when the page is opened in a desktop browser from the PC receiver
 * (receiver/server.js, http://localhost:8080): the same three calls over a WebSocket to the PC receiver, which plays
 * the Cast device.
 */
export function createDevContext(url: string, hooks: DevContextHooks): ReceiverContext {
  const listeners: Record<string, (event: IncomingEvent) => void> = {};
  let socket: WebSocket | null = null;
  const connect = () => {
    socket = new WebSocket(url);
    socket.onopen = () => hooks.onOpen();
    socket.onmessage = (event) => {
      let message: { ctl?: string; ns?: string; data?: unknown; senderId?: string };
      try {
        message = JSON.parse(event.data as string);
      } catch (_) {
        return;
      }
      if (message.ctl === 'reset') {
        hooks.onReset();
        return;
      }
      const listener = message.ns ? listeners[message.ns] : undefined;
      if (message.ctl === 'cast' && listener) listener({ data: message.data, senderId: message.senderId as string });
    };
    socket.onclose = () => setTimeout(connect, 1000);
  };
  return {
    addCustomMessageListener(namespace, listener) {
      listeners[namespace] = listener;
    },
    sendCustomMessage(namespace, senderId, data) {
      if (socket && socket.readyState === 1) socket.send(JSON.stringify({ ctl: 'send', ns: namespace, senderId, data }));
    },
    start: connect,
  };
}
