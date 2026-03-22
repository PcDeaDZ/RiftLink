import 'dart:async';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:sqflite/sqflite.dart';

import 'chat_models.dart';

class ChatRepository {
  ChatRepository._();
  static final ChatRepository instance = ChatRepository._();

  Database? _db;
  String _activeNodeScope = _defaultNodeScope;
  String? _openedNodeScope;
  final _conversationsController = StreamController<void>.broadcast();

  Stream<void> get conversationsChanged => _conversationsController.stream;

  static const int _dbVersion = 3;
  static const String _defaultNodeScope = 'UNBOUND';
  static const String _dbFilePrefix = 'riftlink_chat_';

  static String normalizeNodeScope(String? raw) {
    final trimmed = (raw ?? '').trim();
    if (trimmed.isEmpty) return _defaultNodeScope;
    final hexOnly = trimmed.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
    if (RegExp(r'^[0-9A-F]{16}$').hasMatch(hexOnly)) {
      return hexOnly;
    }
    final safe = trimmed
        .replaceAll(RegExp(r'[^0-9A-Za-z_-]'), '_')
        .replaceAll(RegExp(r'_+'), '_')
        .toUpperCase();
    if (safe.isEmpty) return _defaultNodeScope;
    return safe.length > 64 ? safe.substring(0, 64) : safe;
  }

  String get activeNodeScope => _activeNodeScope;

  Future<void> setActiveNodeScope(String? rawScope) async {
    final next = normalizeNodeScope(rawScope);
    if (next == _activeNodeScope) return;
    _activeNodeScope = next;
    if (_db != null) {
      await _db!.close();
      _db = null;
      _openedNodeScope = null;
    }
    _conversationsController.add(null);
  }

  String _dbFileNameForScope(String scope) {
    return '$_dbFilePrefix$scope.db';
  }

  Future<void> init() async {
    if (_db != null && _openedNodeScope == _activeNodeScope) return;
    if (_db != null) {
      await _db!.close();
      _db = null;
      _openedNodeScope = null;
    }
    final dir = await getApplicationDocumentsDirectory();
    final dbPath = p.join(dir.path, _dbFileNameForScope(_activeNodeScope));
    _db = await openDatabase(
      dbPath,
      version: _dbVersion,
      onCreate: (db, _) async {
        await db.execute('''
          CREATE TABLE conversations (
            id TEXT PRIMARY KEY,
            kind TEXT NOT NULL,
            peer_ref TEXT NOT NULL,
            title TEXT NOT NULL,
            subtitle TEXT,
            last_preview TEXT,
            last_at_ms INTEGER,
            unread_count INTEGER NOT NULL DEFAULT 0,
            archived INTEGER NOT NULL DEFAULT 0,
            pinned INTEGER NOT NULL DEFAULT 0,
            muted INTEGER NOT NULL DEFAULT 0,
            folder_id TEXT NOT NULL DEFAULT 'all'
          )
        ''');
        await db.execute('''
          CREATE TABLE messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id TEXT NOT NULL,
            from_id TEXT NOT NULL,
            to_id TEXT,
            group_id INTEGER,
            group_uid TEXT,
            text TEXT NOT NULL,
            type TEXT NOT NULL,
            lane TEXT NOT NULL,
            direction TEXT NOT NULL,
            status TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            msg_id INTEGER,
            rssi INTEGER,
            delivered INTEGER,
            total INTEGER,
            delete_at_ms INTEGER,
            relay_peers_json TEXT,
            relay_count INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
          )
        ''');
        await db.execute('''
          CREATE TABLE drafts (
            conversation_id TEXT PRIMARY KEY,
            text TEXT NOT NULL DEFAULT ''
          )
        ''');
        await db.execute('''
          CREATE TABLE folders (
            id TEXT PRIMARY KEY,
            title TEXT NOT NULL
          )
        ''');
        await db.execute('''
          CREATE TABLE conversation_pins (
            scope TEXT NOT NULL,
            conversation_id TEXT NOT NULL,
            pin_order INTEGER NOT NULL,
            PRIMARY KEY (scope, conversation_id)
          )
        ''');
        await db.insert('folders', {'id': 'all', 'title': 'All'});
        await db.insert('folders', {'id': 'personal', 'title': 'Personal'});
        await db.insert('folders', {'id': 'groups', 'title': 'Groups'});
        await db.insert('folders', {'id': 'archived', 'title': 'Archived'});

        await db.execute('CREATE INDEX idx_messages_conv_time ON messages(conversation_id, created_at_ms)');
        await db.execute('CREATE INDEX idx_messages_msgid ON messages(msg_id)');
        await db.execute('CREATE INDEX idx_messages_group_uid ON messages(group_uid)');
        await db.execute('CREATE INDEX idx_conversations_unread ON conversations(unread_count)');
        await db.execute('CREATE INDEX idx_conversations_sort ON conversations(pinned, last_at_ms)');
        await db.execute('CREATE INDEX idx_conversation_pins_scope ON conversation_pins(scope, pin_order)');
        await _createGroupSecurityTables(db);
      },
      onUpgrade: (db, oldVersion, newVersion) async {
        if (oldVersion < 2) {
          await db.execute('ALTER TABLE messages ADD COLUMN group_uid TEXT');
          await db.execute('CREATE INDEX IF NOT EXISTS idx_messages_group_uid ON messages(group_uid)');
          await _createGroupSecurityTables(db);
        }
        if (oldVersion < 3) {
          await db.execute('''
            CREATE TABLE IF NOT EXISTS conversation_pins (
              scope TEXT NOT NULL,
              conversation_id TEXT NOT NULL,
              pin_order INTEGER NOT NULL,
              PRIMARY KEY (scope, conversation_id)
            )
          ''');
          await db.execute('CREATE INDEX IF NOT EXISTS idx_conversation_pins_scope ON conversation_pins(scope, pin_order)');
        }
      },
    );
    _openedNodeScope = _activeNodeScope;
  }

  Future<void> _createGroupSecurityTables(Database db) async {
    await db.execute('''
      CREATE TABLE IF NOT EXISTS group_rekey_sessions (
        group_uid TEXT NOT NULL,
        rekey_op_id TEXT NOT NULL,
        key_version INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        deadline INTEGER,
        status TEXT NOT NULL,
        PRIMARY KEY (group_uid, rekey_op_id)
      )
    ''');
    await db.execute('''
      CREATE TABLE IF NOT EXISTS group_member_key_state (
        group_uid TEXT NOT NULL,
        member_id TEXT NOT NULL,
        status TEXT NOT NULL,
        last_try_at INTEGER,
        retry_count INTEGER NOT NULL DEFAULT 0,
        ack_at INTEGER,
        PRIMARY KEY (group_uid, member_id)
      )
    ''');
    await db.execute('''
      CREATE TABLE IF NOT EXISTS group_grants (
        group_uid TEXT NOT NULL,
        subject_id TEXT NOT NULL,
        role TEXT NOT NULL,
        expires_at INTEGER,
        grant_version INTEGER NOT NULL,
        revoked INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (group_uid, subject_id)
      )
    ''');
    await db.execute(
      'CREATE INDEX IF NOT EXISTS idx_group_rekey_status ON group_rekey_sessions(group_uid, status)',
    );
    await db.execute(
      'CREATE INDEX IF NOT EXISTS idx_group_member_state_status ON group_member_key_state(group_uid, status)',
    );
    await db.execute(
      'CREATE INDEX IF NOT EXISTS idx_group_grants_role ON group_grants(group_uid, role, revoked)',
    );
  }

  Future<void> close() async {
    await _db?.close();
    _db = null;
    await _conversationsController.close();
  }

  Future<Database> get _database async {
    await init();
    return _db!;
  }

  static String directConversationId(String peerId) => 'direct:$peerId';
  static String groupConversationIdByUid(String groupUid) => 'groupv2:${groupUid.toUpperCase()}';
  static String broadcastConversationId() => 'broadcast:all';
  static String groupPeerRefByUid(String groupUid) => 'uid:${groupUid.toUpperCase()}';
  static bool isGroupUidPeerRef(String peerRef) => peerRef.toLowerCase().startsWith('uid:');
  static String? groupUidFromPeerRef(String peerRef) {
    if (!isGroupUidPeerRef(peerRef)) return null;
    final raw = peerRef.substring(4).trim();
    return raw.isEmpty ? null : raw.toUpperCase();
  }

  Future<void> ensureConversation({
    required String id,
    required ConversationKind kind,
    required String peerRef,
    required String title,
    String? subtitle,
  }) async {
    final db = await _database;
    await db.insert(
      'conversations',
      {
        'id': id,
        'kind': kind.name,
        'peer_ref': peerRef,
        'title': title,
        'subtitle': subtitle,
      },
      conflictAlgorithm: ConflictAlgorithm.ignore,
    );
  }

  Future<void> upsertConversationMeta(
    String id, {
    String? title,
    String? subtitle,
    String? lastPreview,
    int? lastAtMs,
    int? unreadCount,
    bool? archived,
    bool? pinned,
    bool? muted,
    String? folderId,
  }) async {
    final db = await _database;
    final update = <String, Object?>{};
    if (title != null) update['title'] = title;
    if (subtitle != null) update['subtitle'] = subtitle;
    if (lastPreview != null) update['last_preview'] = lastPreview;
    if (lastAtMs != null) update['last_at_ms'] = lastAtMs;
    if (unreadCount != null) update['unread_count'] = unreadCount;
    if (archived != null) update['archived'] = archived ? 1 : 0;
    if (pinned != null) update['pinned'] = pinned ? 1 : 0;
    if (muted != null) update['muted'] = muted ? 1 : 0;
    if (folderId != null) update['folder_id'] = folderId;
    if (update.isEmpty) return;
    await db.update('conversations', update, where: 'id = ?', whereArgs: [id]);
    _conversationsController.add(null);
  }

  Future<int> appendMessage(ChatMessage message, {bool incrementUnread = false}) async {
    final db = await _database;
    final nowMs = DateTime.now().millisecondsSinceEpoch;
    final insertedId = await db.insert(
      'messages',
      {
        'conversation_id': message.conversationId,
        'from_id': message.from,
        'to_id': message.to,
        'group_id': message.groupId,
        'group_uid': message.groupUid,
        'text': message.text,
        'type': message.type,
        'lane': message.lane,
        'direction': message.direction.name,
        'status': message.status.name,
        'created_at_ms': message.createdAtMs,
        'msg_id': message.msgId,
        'rssi': message.rssi,
        'delivered': message.delivered,
        'total': message.total,
        'delete_at_ms': message.deleteAtMs,
        'relay_peers_json': relayPeersToJson(message.relayPeers),
        'relay_count': message.relayCount,
      },
    );

    final conv = await db.query(
      'conversations',
      columns: ['unread_count'],
      where: 'id = ?',
      whereArgs: [message.conversationId],
      limit: 1,
    );
    final currentUnread = conv.isNotEmpty ? ((conv.first['unread_count'] as int?) ?? 0) : 0;
    await db.update(
      'conversations',
      {
        'last_preview': _buildPreview(message),
        'last_at_ms': message.createdAtMs,
        'unread_count': incrementUnread ? currentUnread + 1 : currentUnread,
      },
      where: 'id = ?',
      whereArgs: [message.conversationId],
    );

    // Auto-fix stale "no timestamp" entries.
    await db.rawUpdate(
      'UPDATE messages SET created_at_ms = ? WHERE conversation_id = ? AND created_at_ms <= 0',
      [nowMs, message.conversationId],
    );
    _conversationsController.add(null);
    return insertedId;
  }

  String _buildPreview(ChatMessage message) {
    if (message.type == 'voice') return 'Voice message';
    if (message.type == 'location') return 'Location';
    return message.text;
  }

  Future<List<ChatConversation>> listConversations({
    bool includeArchived = false,
    String folderId = 'all',
    String query = '',
  }) async {
    final db = await _database;
    final where = <String>[];
    final args = <Object?>[];
    if (!includeArchived) {
      where.add('archived = 0');
    }
    if (folderId != 'all') {
      where.add('folder_id = ?');
      args.add(folderId);
    }
    if (query.trim().isNotEmpty) {
      where.add('(title LIKE ? OR last_preview LIKE ?)');
      final q = '%${query.trim()}%';
      args.add(q);
      args.add(q);
    }

    final rows = await db.query(
      'conversations',
      where: where.isEmpty ? null : where.join(' AND '),
      whereArgs: args,
      orderBy: 'pinned DESC, last_at_ms DESC',
    );
    return rows.map(_conversationFromRow).toList();
  }

  Future<List<ChatConversation>> listArchivedConversations() async {
    final db = await _database;
    final rows = await db.query(
      'conversations',
      where: 'archived = 1',
      // Archived list is timeline-based: newest archived chats on top.
      // Scope pins are folder-specific and should not affect archive ordering.
      orderBy: 'last_at_ms DESC',
    );
    return rows.map(_conversationFromRow).toList();
  }

  Future<ChatConversation?> getConversation(String conversationId) async {
    final db = await _database;
    final rows = await db.query('conversations', where: 'id = ?', whereArgs: [conversationId], limit: 1);
    if (rows.isEmpty) return null;
    return _conversationFromRow(rows.first);
  }

  Future<List<ChatMessage>> listMessages(String conversationId, {int limit = 300}) async {
    final db = await _database;
    final rows = await db.query(
      'messages',
      where: 'conversation_id = ?',
      whereArgs: [conversationId],
      orderBy: 'created_at_ms ASC',
      limit: limit,
    );
    return rows.map(_messageFromRow).toList();
  }

  Future<void> markConversationRead(String conversationId) async {
    final db = await _database;
    await db.update(
      'conversations',
      {'unread_count': 0},
      where: 'id = ?',
      whereArgs: [conversationId],
    );
    _conversationsController.add(null);
  }

  Future<void> setPinned(String conversationId, bool value) async {
    await upsertConversationMeta(conversationId, pinned: value);
  }

  Future<Map<String, int>> listPinnedByScope(String scope) async {
    final db = await _database;
    final rows = await db.query(
      'conversation_pins',
      columns: ['conversation_id', 'pin_order'],
      where: 'scope = ?',
      whereArgs: [scope],
      orderBy: 'pin_order ASC',
    );
    final out = <String, int>{};
    for (final row in rows) {
      final id = (row['conversation_id'] as String?) ?? '';
      if (id.isEmpty) continue;
      out[id] = (row['pin_order'] as int?) ?? 0;
    }
    return out;
  }

  Future<void> setPinnedForScope({
    required String conversationId,
    required String scope,
    required bool value,
  }) async {
    final db = await _database;
    final normScope = scope.trim().toLowerCase();
    if (normScope.isEmpty) return;
    await db.transaction((txn) async {
      if (!value) {
        await txn.delete(
          'conversation_pins',
          where: 'scope = ? AND conversation_id = ?',
          whereArgs: [normScope, conversationId],
        );
        return;
      }
      final existing = await txn.query(
        'conversation_pins',
        columns: ['pin_order'],
        where: 'scope = ? AND conversation_id = ?',
        whereArgs: [normScope, conversationId],
        limit: 1,
      );
      if (existing.isNotEmpty) return;
      final maxRow = await txn.rawQuery(
        'SELECT COALESCE(MAX(pin_order), 0) AS max_order FROM conversation_pins WHERE scope = ?',
        [normScope],
      );
      final maxOrder = (maxRow.first['max_order'] as int?) ?? 0;
      await txn.insert(
        'conversation_pins',
        {
          'scope': normScope,
          'conversation_id': conversationId,
          'pin_order': maxOrder + 1,
        },
        conflictAlgorithm: ConflictAlgorithm.replace,
      );
    });
    _conversationsController.add(null);
  }

  Future<void> setArchived(String conversationId, bool value) async {
    await upsertConversationMeta(
      conversationId,
      archived: value,
      // Keep archived list intuitive: newly archived chats appear on top.
      lastAtMs: value ? DateTime.now().millisecondsSinceEpoch : null,
      folderId: value ? 'archived' : 'all',
    );
  }

  Future<void> setMuted(String conversationId, bool value) async {
    await upsertConversationMeta(conversationId, muted: value);
  }

  Future<void> setFolder(String conversationId, String folderId) async {
    await upsertConversationMeta(conversationId, folderId: folderId);
  }

  Future<void> deleteConversation(String conversationId) async {
    final db = await _database;
    await db.delete('messages', where: 'conversation_id = ?', whereArgs: [conversationId]);
    await db.delete('drafts', where: 'conversation_id = ?', whereArgs: [conversationId]);
    await db.delete('conversation_pins', where: 'conversation_id = ?', whereArgs: [conversationId]);
    await db.delete('conversations', where: 'id = ?', whereArgs: [conversationId]);
    _conversationsController.add(null);
  }

  Future<void> clearAll() async {
    final db = await _database;
    await db.delete('messages');
    await db.delete('drafts');
    await db.delete('conversation_pins');
    await db.delete('conversations');
    _conversationsController.add(null);
  }

  Future<void> setDraft(String conversationId, String text) async {
    final db = await _database;
    await db.insert(
      'drafts',
      {
        'conversation_id': conversationId,
        'text': text,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
  }

  Future<String> getDraft(String conversationId) async {
    final db = await _database;
    final rows = await db.query('drafts', where: 'conversation_id = ?', whereArgs: [conversationId], limit: 1);
    if (rows.isEmpty) return '';
    return (rows.first['text'] as String?) ?? '';
  }

  Future<void> updateByMsgId({
    required int msgId,
    required String conversationId,
    MessageStatus? status,
    int? delivered,
    int? total,
  }) async {
    final db = await _database;
    final payload = <String, Object?>{};
    if (status != null) payload['status'] = status.name;
    if (delivered != null) payload['delivered'] = delivered;
    if (total != null) payload['total'] = total;
    if (payload.isEmpty) return;
    await db.update(
      'messages',
      payload,
      where: 'conversation_id = ? AND msg_id = ?',
      whereArgs: [conversationId, msgId],
    );
  }

  Future<void> upsertGroupRekeySession({
    required String groupUid,
    required String rekeyOpId,
    required int keyVersion,
    required int createdAt,
    int? deadline,
    required String status,
  }) async {
    final db = await _database;
    await db.insert(
      'group_rekey_sessions',
      {
        'group_uid': groupUid.toUpperCase(),
        'rekey_op_id': rekeyOpId,
        'key_version': keyVersion,
        'created_at': createdAt,
        'deadline': deadline,
        'status': status,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
  }

  Future<void> upsertGroupMemberKeyState({
    required String groupUid,
    required String memberId,
    required String status,
    int? lastTryAt,
    int? retryCount,
    int? ackAt,
  }) async {
    final db = await _database;
    await db.insert(
      'group_member_key_state',
      {
        'group_uid': groupUid.toUpperCase(),
        'member_id': memberId.toUpperCase(),
        'status': status,
        'last_try_at': lastTryAt,
        'retry_count': retryCount ?? 0,
        'ack_at': ackAt,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
  }

  Future<void> upsertGroupGrant({
    required String groupUid,
    required String subjectId,
    required String role,
    int? expiresAt,
    required int grantVersion,
    required bool revoked,
  }) async {
    final db = await _database;
    await db.insert(
      'group_grants',
      {
        'group_uid': groupUid.toUpperCase(),
        'subject_id': subjectId.toUpperCase(),
        'role': role,
        'expires_at': expiresAt,
        'grant_version': grantVersion,
        'revoked': revoked ? 1 : 0,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
  }

  Future<void> migrateLegacyGroupConversationsToUid(Map<int, String> groupIdToUid) async {
    if (groupIdToUid.isEmpty) return;
    final db = await _database;
    await db.transaction((txn) async {
      for (final entry in groupIdToUid.entries) {
        final gid = entry.key;
        final uid = entry.value.trim().toUpperCase();
        if (gid <= 1 || uid.isEmpty) continue;
        final legacyId = 'group:$gid';
        final v2Id = groupConversationIdByUid(uid);
        final v2PeerRef = groupPeerRefByUid(uid);

        final legacyRows = await txn.query(
          'conversations',
          columns: ['id', 'title', 'subtitle', 'last_preview', 'last_at_ms', 'unread_count', 'archived', 'pinned', 'muted', 'folder_id'],
          where: 'id = ?',
          whereArgs: [legacyId],
          limit: 1,
        );
        if (legacyRows.isEmpty) continue;

        final existingV2Rows = await txn.query(
          'conversations',
          columns: ['id'],
          where: 'id = ?',
          whereArgs: [v2Id],
          limit: 1,
        );

        if (existingV2Rows.isEmpty) {
          final legacy = legacyRows.first;
          await txn.insert(
            'conversations',
            {
              'id': v2Id,
              'kind': 'group',
              'peer_ref': v2PeerRef,
              'title': (legacy['title'] as String?) ?? 'Group $gid',
              'subtitle': legacy['subtitle'],
              'last_preview': legacy['last_preview'],
              'last_at_ms': legacy['last_at_ms'],
              'unread_count': legacy['unread_count'] ?? 0,
              'archived': legacy['archived'] ?? 0,
              'pinned': legacy['pinned'] ?? 0,
              'muted': legacy['muted'] ?? 0,
              'folder_id': legacy['folder_id'] ?? 'all',
            },
            conflictAlgorithm: ConflictAlgorithm.ignore,
          );
        }

        await txn.update(
          'messages',
          {
            'conversation_id': v2Id,
            'group_uid': uid,
          },
          where: 'conversation_id = ?',
          whereArgs: [legacyId],
        );
        final legacyDraft = await txn.query(
          'drafts',
          columns: ['text'],
          where: 'conversation_id = ?',
          whereArgs: [legacyId],
          limit: 1,
        );
        if (legacyDraft.isNotEmpty) {
          await txn.insert(
            'drafts',
            {
              'conversation_id': v2Id,
              'text': (legacyDraft.first['text'] as String?) ?? '',
            },
            conflictAlgorithm: ConflictAlgorithm.replace,
          );
          await txn.delete('drafts', where: 'conversation_id = ?', whereArgs: [legacyId]);
        }
        await txn.delete('conversations', where: 'id = ?', whereArgs: [legacyId]);
      }
    });
    _conversationsController.add(null);
  }

  ChatConversation _conversationFromRow(Map<String, Object?> row) {
    return ChatConversation(
      id: row['id'] as String,
      kind: ConversationKind.values.firstWhere(
        (v) => v.name == row['kind'],
        orElse: () => ConversationKind.direct,
      ),
      peerRef: row['peer_ref'] as String,
      title: (row['title'] as String?) ?? '',
      subtitle: row['subtitle'] as String?,
      lastMessagePreview: row['last_preview'] as String?,
      lastMessageAtMs: row['last_at_ms'] as int?,
      unreadCount: (row['unread_count'] as int?) ?? 0,
      archived: ((row['archived'] as int?) ?? 0) == 1,
      pinned: ((row['pinned'] as int?) ?? 0) == 1,
      muted: ((row['muted'] as int?) ?? 0) == 1,
      folderId: (row['folder_id'] as String?) ?? 'all',
    );
  }

  ChatMessage _messageFromRow(Map<String, Object?> row) {
    return ChatMessage(
      id: row['id'] as int?,
      conversationId: row['conversation_id'] as String,
      from: row['from_id'] as String? ?? '',
      to: row['to_id'] as String?,
      groupId: row['group_id'] as int?,
      groupUid: row['group_uid'] as String?,
      text: row['text'] as String? ?? '',
      type: row['type'] as String? ?? 'text',
      lane: row['lane'] as String? ?? 'normal',
      direction: MessageDirection.values.firstWhere(
        (v) => v.name == row['direction'],
        orElse: () => MessageDirection.incoming,
      ),
      status: MessageStatus.values.firstWhere(
        (v) => v.name == row['status'],
        orElse: () => MessageStatus.pending,
      ),
      createdAtMs: row['created_at_ms'] as int? ?? 0,
      msgId: row['msg_id'] as int?,
      rssi: row['rssi'] as int?,
      delivered: row['delivered'] as int?,
      total: row['total'] as int?,
      deleteAtMs: row['delete_at_ms'] as int?,
      relayPeers: relayPeersFromJson(row['relay_peers_json'] as String?),
      relayCount: (row['relay_count'] as int?) ?? 0,
    );
  }
}

